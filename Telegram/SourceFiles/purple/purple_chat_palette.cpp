#include "purple/purple_chat_palette.h"

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "boxes/peer_list_box.h"
#include "core/application.h"
#include "crl/crl_on_main.h"
#include "data/data_folder.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "mtproto/sender.h"
#include "purple/purple_config.h"
#include "purple/purple_chat_palette_search.h"
#include "purple/purple_keys.h"
#include "purple/purple_picker.h"
#include "storage/storage_account.h"
#include "ui/painter.h"
#include "ui/text/text_options.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_purple_chat_palette.h"
#include "styles/style_widgets.h"

#include <QtCore/QRegularExpression>
#include <deque>

namespace Purple {
namespace {

constexpr auto kSearchDelay = crl::time(250);
constexpr auto kNoteInterval = crl::time(500);

bool Current(not_null<Window::SessionController*> controller) {
	const auto &session = controller->session();
	return !Core::App().passcodeLocked()
		&& !controller->window().locked()
		&& controller->window().sessionController() == controller
		&& !session.account().loggingOut()
		&& session.account().maybeSession() == &session;
}

QString PhoneDigits(const QString &query) {
	static const auto format = QRegularExpression(uR"(^\+?[\d\s().-]+$)"_q);
	if (!format.match(query).hasMatch()) return {};
	auto digits = QString();
	for (const auto character : query) {
		const auto digit = character.digitValue();
		if (digit >= 0) digits.append(QChar(u'0' + digit));
	}
	return digits;
}

bool Searchable(not_null<PeerData*> peer) {
	return !peer->migrateTo() && PickerVisible(peer);
}

constexpr auto kRecentChatsPref = "purple-chat-palette-visits";
constexpr auto kMaxRecentChats = size_t(128);

std::vector<uint64> ReadRecentChats(Main::Session &session) {
	const auto serialized = session.local().readPref<QByteArray>(
		kRecentChatsPref,
		QByteArray());
	auto result = std::vector<uint64>();
	for (const auto &part : serialized.split(',')) {
		const auto value = part.toULongLong();
		if (value && !ranges::contains(result, value)) {
			result.push_back(value);
			if (result.size() == kMaxRecentChats) {
				break;
			}
		}
	}
	return result;
}

QString PeerLabel(not_null<PeerData*> peer) {
	return peer->isSelf() ? tr::lng_saved_messages(tr::now) : peer->name();
}

std::optional<int> Rank(
		not_null<PeerData*> peer,
		const std::vector<ChatPaletteSearch::Term> &terms,
		const QString &phone) {
	const auto user = peer->asUser();
	if (user && !phone.isEmpty()
		&& ChatPaletteSearch::Fold(user->phone()).text.contains(phone)) {
		return 6;
	}
	const auto username = peer->username();
	const auto address = (username.isEmpty() ? QString() : u'@' + username)
		+ (user ? u' ' + user->phone() : QString());
	return ChatPaletteSearch::Rank(
		terms,
		PeerLabel(peer),
		address,
		user ? user->note().text : QString());
}

struct SearchState {
	std::vector<ChatPaletteSearch::Term> terms;
	base::flat_map<uint64, int> order;
	base::flat_map<uint64, int> ranks;
};

class ChatRow final : public PeerListRow {
public:
	ChatRow(not_null<PeerData*> peer, std::shared_ptr<SearchState> search)
	: PeerListRow(peer)
	, _search(std::move(search)) {
	}

	void paintStatusText(
			Painter &p,
			const style::PeerListItem &st,
			int x,
			int y,
			int availableWidth,
			int outerWidth,
			bool selected) override {
		const auto user = peer()->asUser();
		if (!user || user->note().text.isEmpty()) {
			PeerListRow::paintStatusText(p, st, x, y, availableWidth, outerWidth, selected);
			return;
		}
		auto note = TextWithEntities{ user->note().text };
		const auto folded = ChatPaletteSearch::Fold(note.text, true);
		for (const auto &term : _search->terms) {
			if (term.literal.isEmpty()) {
				continue;
			}
			for (auto i = folded.text.indexOf(term.literal); i >= 0;
				i = folded.text.indexOf(term.literal, i + term.literal.size())) {
				const auto from = folded.positions[i];
				const auto end = folded.positions[i + term.literal.size() - 1] + 1;
				note.entities.append(EntityInText(EntityType::Bold, from, end - from));
			}
		}
		ranges::sort(note.entities, std::less<>(), &EntityInText::offset);
		auto text = Ui::Text::String();
		text.setMarkedText(st::defaultTextStyle, note, Ui::NameTextOptions());
		p.setFont(st::contactsStatusFont);
		p.setPen(selected ? st.statusFgOver : st.statusFg);
		text.drawLeftElided(p, x, y, availableWidth, outerWidth);
	}

private:
	std::shared_ptr<SearchState> _search;
};

class ChatPaletteBox final : public PeerListBox {
public:
	using PeerListBox::PeerListBox;

	void sortResults(Fn<bool(const PeerListRow&, const PeerListRow&)> compare) {
		content()->sortSearchRows(std::move(compare));
	}
};

void ForEachChat(
		not_null<Main::Session*> session,
		Fn<void(not_null<PeerData*>)> add) {
	auto &data = session->data();
	auto seen = base::flat_set<PeerId>();
	const auto append = [&](not_null<Dialogs::IndexedList*> chats) {
		for (const auto &row : chats->all()) {
			const auto history = row->history();
			if (history && seen.insert(history->peer->id).second
				&& Searchable(history->peer)) {
				add(history->peer);
			}
		}
	};
	append(data.chatsList()->indexed());
	if (const auto folder = data.folderLoaded(Data::Folder::kId)) {
		append(folder->chatsList()->indexed());
	}
	append(data.contactsList());
	if (!seen.contains(session->user()->id) && Searchable(session->user())) {
		add(session->user());
	}
}

class ChatSearch final : public PeerListSearchController {
public:
	ChatSearch(
		not_null<Window::SessionController*> window,
		std::shared_ptr<SearchState> state)
	: _state(std::move(state))
	, _window(base::make_weak(window.get()))
	, _api(&window->session().mtp())
	, _timer([=] { resolve(); })
	, _noteTimer([=] { loadNextNote(); }) {
	}

	void searchQuery(const QString &raw) override {
		_api.request(base::take(_request)).cancel();
		_timer.cancel();
		_noteTimer.cancel();
		_pendingNotes.clear();
		_query = raw.trimmed();
		_state->terms = ChatPaletteSearch::Parse(_query);
		_state->ranks.clear();
		const auto window = _window.get();
		if (_query.isEmpty() || !window || !Current(window)) return;
		const auto phone = PhoneDigits(_query);
		const auto searchNotes = phone.isEmpty() && !_query.startsWith(u'@');
		auto foundPhone = false;
		ForEachChat(&window->session(), [&](not_null<PeerData*> peer) {
			const auto user = peer->asUser();
			if (const auto rank = Rank(peer, _state->terms, phone)) {
				addResult(peer, *rank);
				foundPhone |= user && user->phone() == phone;
			}
			if (searchNotes && user && user->isContact() && !user->isSelf()
				&& !user->wasFullUpdated() && !_notesAttempted.contains(user->id)) {
				_pendingNotes.push_back(user->id);
			}
		});
		static const auto username = QRegularExpression(uR"(^@[a-zA-Z0-9_]+$)"_q);
		if ((!foundPhone && _query.startsWith(u'+') && phone.size() >= 7)
			|| username.match(_query).hasMatch()) {
			_timer.callOnce(kSearchDelay);
		}
		scheduleNote();
		delegate()->peerListSearchRefreshRows();
	}

	bool isLoading() override {
		return _timer.isActive() || _request || _noteTimer.isActive() || _noteRequest;
	}
	bool loadMoreRows() override { return false; }

private:
	void addResult(not_null<PeerData*> peer, int rank) {
		_state->ranks[peer->id.value] = rank;
		delegate()->peerListSearchAddRow(peer);
	}

	void scheduleNote() {
		if (!_noteRequest && !_pendingNotes.empty()) {
			_noteTimer.callOnce(kNoteInterval);
		}
	}

	void loadNextNote() {
		const auto window = _window.get();
		if (!window || !Current(window)) return;
		auto &data = window->session().data();
		while (!_pendingNotes.empty()) {
			const auto id = _pendingNotes.front();
			_pendingNotes.pop_front();
			const auto user = data.userLoaded(peerToUser(id));
			if (!user || !user->isContact() || user->wasFullUpdated()
				|| !Searchable(user)) continue;
			_notesAttempted.insert(id);
			_noteRequest = _api.request(MTPusers_GetFullUser(user->inputUser()))
			.done([=](const MTPusers_UserFull &result) {
				_noteRequest = 0;
				const auto live = _window.get();
				if (!live || !Current(live)) return;
				if (user->isContact() && Searchable(user) && !user->wasFullUpdated()) {
					const auto &full = result.c_users_userFull();
					const auto &fields = full.vfull_user().c_userFull();
					if (fields.vid().v == int64(peerToUser(id).bare)) {
						auto &owner = live->session().data();
						owner.processUsers(full.vusers());
						owner.processChats(full.vchats());
						Data::ApplyUserUpdate(user, fields);
					}
				}
				if (!_query.isEmpty() && !_query.startsWith(u'@')
					&& PhoneDigits(_query).isEmpty() && Searchable(user)) {
					if (const auto rank = Rank(user, _state->terms, QString())) {
						addResult(user, *rank);
					}
				}
				scheduleNote();
				delegate()->peerListSearchRefreshRows();
			}).fail([=](const MTP::Error &) {
				_noteRequest = 0;
				scheduleNote();
				delegate()->peerListSearchRefreshRows();
			}).send();
			return;
		}
		delegate()->peerListSearchRefreshRows();
	}

	void resolve() {
		const auto window = _window.get();
		if (!window || !Current(window)) return;
		const auto phone = _query.startsWith(u'+') ? PhoneDigits(_query) : QString();
		const auto send = [&](auto request) {
			_request = _api.request(std::move(request)).done(
				[=](const MTPcontacts_ResolvedPeer &result) {
					_request = 0;
					const auto live = _window.get();
					if (!live || !Current(live)) return;
					auto &data = live->session().data();
					const auto &resolved = result.c_contacts_resolvedPeer();
					data.processUsers(resolved.vusers());
					data.processChats(resolved.vchats());
					const auto peer = data.peerLoaded(peerFromMTP(resolved.vpeer()));
					if (peer && Searchable(peer)) {
						addResult(peer, Rank(peer, _state->terms, phone).value_or(6));
					}
					delegate()->peerListSearchRefreshRows();
				}).fail([=](const MTP::Error &) {
					_request = 0;
					delegate()->peerListSearchRefreshRows();
				}).send();
		};
		if (!phone.isEmpty()) {
			send(MTPcontacts_ResolvePhone(MTP_string(phone)));
		} else {
			send(MTPcontacts_ResolveUsername(
				MTP_flags(0), MTP_string(_query.mid(1)), MTPstring()));
		}
	}

	std::shared_ptr<SearchState> _state;
	base::weak_ptr<Window::SessionController> _window;
	MTP::Sender _api;
	base::Timer _timer;
	base::Timer _noteTimer;
	std::deque<PeerId> _pendingNotes;
	base::flat_set<PeerId> _notesAttempted;
	mtpRequestId _request = 0;
	mtpRequestId _noteRequest = 0;
	QString _query;
};

class ChatPalette final : public PeerListController {
public:
	ChatPalette(
		not_null<Window::SessionController*> window,
		bool recent,
		std::shared_ptr<SearchState> search = std::make_shared<SearchState>())
	: PeerListController(std::make_unique<ChatSearch>(window, search))
	, _window(base::make_weak(window.get()))
	, _session(&window->session())
	, _search(std::move(search))
	, _recent(recent) {
	}

	void setBox(not_null<ChatPaletteBox*> box) { _box = box.get(); }

	Main::Session &session() const override { return *_session; }
	bool searchInLocal() override { return false; }

	int contentWidth() const override {
		const auto window = _window.get();
		const auto width = window ? window->widget()->bodyWidget()->width() : 0;
		return std::clamp(width - st::purpleChatPaletteInset,
			st::boxWideWidth, st::purpleChatPaletteMaxWidth);
	}

	void peerListSearchRefreshRows() override {
		if (_box) {
			_box->sortResults([&](const PeerListRow &a, const PeerListRow &b) {
				return sortKey(a.id()) < sortKey(b.id());
			});
		}
		PeerListController::peerListSearchRefreshRows();
	}

	rpl::producer<QString> searchPlaceholder() const override {
		return tr::lng_purple_chat_palette_search();
	}

	void prepare() override {
		delegate()->peerListSetTitle(tr::lng_purple_chat_palette());
		delegate()->peerListSetSearchMode(PeerListSearchMode::Enabled);
		setSearchNoResultsText(tr::lng_purple_chat_palette_empty(tr::now));
		auto peers = std::vector<not_null<PeerData*>>();
		ForEachChat(_session, [&](not_null<PeerData*> peer) {
			peers.push_back(peer);
		});
		if (_recent) {
			const auto window = _window.get();
			const auto active = window ? window->activeChatCurrent().peer() : nullptr;
			const auto recent = ReadRecentChats(*_session);
			const auto key = [&](not_null<PeerData*> peer) {
				return peer == active
					? recent.size() + 1
					: size_t(ranges::find(recent, peer->id.value) - recent.begin());
			};
			std::stable_sort(peers.begin(), peers.end(), [&](auto a, auto b) {
				return key(a) < key(b);
			});
		}
		for (const auto peer : peers) {
			_search->order[peer->id.value] = int(_search->order.size());
			delegate()->peerListAppendRow(createSearchRow(peer));
		}
		delegate()->peerListRefreshRows();
	}

	std::unique_ptr<PeerListRow> createSearchRow(
			not_null<PeerData*> peer) override {
		if (!Searchable(peer)) return {};
		auto row = std::make_unique<ChatRow>(peer, _search);
		if (peer->isSelf()) {
			row->setSavedMessagesChatStatus(tr::lng_saved_messages(tr::now));
		} else if (const auto user = peer->asUser(); user && !user->note().text.isEmpty()) {
			row->setCustomStatus(user->note().text);
		} else if (const auto username = peer->username(); !username.isEmpty()) {
			row->setCustomStatus(u'@' + username);
		}
		return row;
	}

	void rowClicked(not_null<PeerListRow*> row) override {
		const auto window = _window;
		const auto live = window.get();
		const auto peer = row->peer();
		if (!_box || !live || !Current(live) || !Searchable(peer)) return;
		const auto id = peer->id;
		_box->closeBox();
		crl::on_main(live, [window, id] {
			const auto controller = window.get();
			if (!controller || !Current(controller)
				|| Core::App().activeWindow() != &controller->window()) return;
			const auto peer = controller->session().data().peerLoaded(id);
			if (peer && Searchable(peer)) controller->showPeerHistory(peer);
		});
	}

private:
	std::pair<int, int> sortKey(uint64 id) const {
		const auto rank = _search->ranks.find(id);
		const auto order = _search->order.find(id);
		return {
			(rank == _search->ranks.end()) ? 8 : rank->second,
			(order == _search->order.end()) ? int(_search->order.size()) : order->second,
		};
	}

	base::weak_ptr<Window::SessionController> _window;
	const not_null<Main::Session*> _session;
	std::shared_ptr<SearchState> _search;
	const bool _recent;
	QPointer<ChatPaletteBox> _box;
};

} // namespace

bool IsChatPaletteController(const PeerListController *controller) {
	return dynamic_cast<const ChatPalette*>(controller) != nullptr;
}

void RecordChatPaletteVisit(not_null<PeerData*> peer) {
	auto &session = peer->session();
	if (session.account().loggingOut()) {
		return;
	}
	auto recent = ReadRecentChats(session);
	if (!recent.empty() && recent.front() == peer->id.value) {
		return;
	}
	std::erase(recent, peer->id.value);
	recent.insert(recent.begin(), peer->id.value);
	if (recent.size() > kMaxRecentChats) {
		recent.pop_back();
	}
	auto encoded = QByteArray();
	for (const auto id : recent) {
		if (!encoded.isEmpty()) {
			encoded += ',';
		}
		encoded += QByteArray::number(id);
	}
	session.local().writePref<QByteArray>(kRecentChatsPref, encoded);
}

void ShowChatPalette(not_null<Window::SessionController*> controller, bool recent) {
	if (!Current(controller)) return;
	auto palette = std::make_unique<ChatPalette>(controller, recent);
	const auto raw = palette.get();
	controller->show(Box<ChatPaletteBox>(std::move(palette), [=](not_null<PeerListBox*> box) {
		raw->setBox(static_cast<ChatPaletteBox*>(box.get()));
		StateChanges() | rpl::on_next([=] { box->closeBox(); }, box->lifetime());
		ModalKeysEnabledChanges() | rpl::on_next([=](bool enabled) {
			if (!enabled) {
				box->closeBox();
			}
		}, box->lifetime());
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

} // namespace Purple

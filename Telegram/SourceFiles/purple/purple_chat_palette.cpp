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
#include "mtproto/sender.h"
#include "purple/purple_config.h"
#include "purple/purple_exact_search.h"
#include "purple/purple_picker.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

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

bool Matches(
		not_null<PeerData*> peer,
		const QStringList &terms,
		const QString &phone) {
	auto label = peer->isSelf()
		? tr::lng_saved_messages(tr::now)
		: peer->name();
	const auto username = peer->username();
	if (!username.isEmpty()) label += u" @"_q + username;
	if (const auto user = peer->asUser()) {
		if (!phone.isEmpty() && user->phone().contains(phone)) return true;
		label += u' ' + user->note().text;
	}
	return MatchExactSearch(terms, label);
}

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
	explicit ChatSearch(not_null<Window::SessionController*> window)
	: _window(base::make_weak(window.get()))
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
		const auto window = _window.get();
		if (_query.isEmpty() || !window || !Current(window)) return;
		const auto terms = ExactSearchTerms(_query);
		const auto phone = PhoneDigits(_query);
		const auto searchNotes = phone.isEmpty() && !_query.startsWith(u'@');
		auto foundPhone = false;
		ForEachChat(&window->session(), [&](not_null<PeerData*> peer) {
			const auto user = peer->asUser();
			if (Matches(peer, terms, phone)) {
				delegate()->peerListSearchAddRow(peer);
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
					&& PhoneDigits(_query).isEmpty() && Searchable(user)
					&& Matches(user, ExactSearchTerms(_query), QString())) {
					delegate()->peerListSearchAddRow(user);
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
					if (peer && Searchable(peer)) delegate()->peerListSearchAddRow(peer);
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
	explicit ChatPalette(not_null<Window::SessionController*> window)
	: PeerListController(std::make_unique<ChatSearch>(window))
	, _window(base::make_weak(window.get()))
	, _session(&window->session()) {
	}

	void setBox(not_null<PeerListBox*> box) { _box = box.get(); }

	Main::Session &session() const override { return *_session; }
	bool searchInLocal() override { return false; }

	rpl::producer<QString> searchPlaceholder() const override {
		return tr::lng_purple_chat_palette_search();
	}

	void prepare() override {
		delegate()->peerListSetTitle(tr::lng_purple_chat_palette());
		delegate()->peerListSetSearchMode(PeerListSearchMode::Enabled);
		setSearchNoResultsText(tr::lng_purple_chat_palette_empty(tr::now));
		ForEachChat(_session, [&](not_null<PeerData*> peer) {
			delegate()->peerListAppendRow(createSearchRow(peer));
		});
		delegate()->peerListRefreshRows();
	}

	std::unique_ptr<PeerListRow> createSearchRow(
			not_null<PeerData*> peer) override {
		if (!Searchable(peer)) return {};
		auto row = std::make_unique<PeerListRow>(peer);
		if (peer->isSelf()) {
			row->setSavedMessagesChatStatus(tr::lng_saved_messages(tr::now));
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
	base::weak_ptr<Window::SessionController> _window;
	const not_null<Main::Session*> _session;
	QPointer<PeerListBox> _box;
};

} // namespace

bool IsChatPaletteController(const PeerListController *controller) {
	return dynamic_cast<const ChatPalette*>(controller) != nullptr;
}

void ShowChatPalette(not_null<Window::SessionController*> controller) {
	if (!Current(controller)) return;
	auto palette = std::make_unique<ChatPalette>(controller);
	const auto raw = palette.get();
	controller->show(Box<PeerListBox>(std::move(palette), [=](not_null<PeerListBox*> box) {
		raw->setBox(box);
		StateChanges() | rpl::on_next([=] { box->closeBox(); }, box->lifetime());
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

} // namespace Purple

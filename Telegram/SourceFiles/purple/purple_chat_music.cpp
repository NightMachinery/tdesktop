#include "purple/purple_chat_music.h"

#include "base/weak_ptr.h"
#include "core/application.h"
#include "data/data_chat.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_shared_media.h"
#include "data/data_thread.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "history/view/history_view_context_menu.h"
#include "history/view/history_view_element.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "mainwidget.h"
#include "media/audio/media_audio.h"
#include "media/player/media_player_instance.h"
#include "purple/purple_exact_search.h"
#include "purple/purple_keys.h"
#include "purple/purple_playlist_song_list.h"
#include "purple/purple_playlists_box.h"
#include "ui/layers/generic_box.h"
#include "ui/text/format_values.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_controller.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"

#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_purple_music.h"
#include "styles/style_widgets.h"

#include <QtGui/QKeyEvent>
#include <QtGui/QTextBlock>
#include <QtGui/QTextLayout>
#include <QtWidgets/QTextEdit>

namespace Purple {
namespace {

constexpr auto kPageSize = 100;

struct SharedTrack {
	FullMsgId message;
	PlaylistSongListRow row;
	QString searchText;
};

class ChatMusicBox final : public QObject {
public:
	ChatMusicBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		SparseIdsMergedSlice::Key source);

protected:
	bool eventFilter(QObject *object, QEvent *event) override;

private:
	struct Anchor {
		uint64_t cursor = 0;
		int scrollTop = 0;
	};

	[[nodiscard]] bool current() const;
	[[nodiscard]] uint64_t rowKey(FullMsgId id) const;
	[[nodiscard]] const SharedTrack *track(uint64_t key) const;
	[[nodiscard]] HistoryItem *item(uint64_t key) const;
	void load();
	void apply(const SparseIdsMergedSlice &slice);
	void render(std::optional<Anchor> anchor = {});
	void updatePlayback();
	void filter();
	void focusList();
	bool keyPress(QKeyEvent *event);
	void play(uint64_t key);
	void forward(uint64_t key);
	void goTo(uint64_t key);
	void addToPlaylist(uint64_t key);
	void copyLink(uint64_t key);
	void fillActions(not_null<Ui::PopupMenu*> menu, uint64_t key);
	void mark(uint64_t key);
	void setStatus(const QString &text);

	const not_null<Ui::GenericBox*> _box;
	const base::weak_ptr<Window::SessionController> _controller;
	const not_null<Main::Session*> _session;
	const PeerId _peer;
	const MsgId _topic;
	const PeerId _sublist;
	const PeerId _migrated;
	not_null<Ui::InputField*> _search;
	not_null<Ui::FlatLabel*> _status;
	not_null<PlaylistSongList*> _list;
	QPointer<Ui::RoundButton> _more;
	std::vector<SharedTrack> _tracks;
	std::set<uint64_t> _marked;
	std::optional<Anchor> _unfiltered;
	QStringList _terms;
	rpl::lifetime _viewer;
	int _limit = kPageSize;
	bool _loading = true;
	bool _hasMore = false;
	bool _pendingG = false;

};

ChatMusicBox::ChatMusicBox(
	not_null<Ui::GenericBox*> box,
	not_null<Window::SessionController*> controller,
		SparseIdsMergedSlice::Key source)
: QObject(box)
, _box(box)
, _controller(base::make_weak(controller.get()))
, _session(&controller->session())
, _peer(source.peerId)
, _topic(source.topicRootId)
, _sublist(source.monoforumPeerId)
, _migrated(source.migratedPeerId)
, _search(box->addRow(object_ptr<Ui::InputField>(
	box, st::defaultInputField, Ui::InputField::Mode::SingleLine,
	tr::lng_purple_music_search())))
, _status(box->addRow(object_ptr<Ui::FlatLabel>(
	box, tr::lng_contacts_loading(tr::now), st::boxLabel)))
, _list(box->addRow(object_ptr<PlaylistSongList>(
	box,
	std::vector<PlaylistSongListRow>(),
	0,
	0,
	[=](uint64_t key) { play(key); },
	Fn<void(std::vector<uint64_t>)>(),
	PlaylistSongList::Mode::Library))) {
	_search->rawTextEdit()->installEventFilter(this);
	_search->changes() | rpl::on_next([=] { filter(); }, box->lifetime());
	_search->submits() | rpl::on_next([=] { focusList(); }, box->lifetime());
	_list->setViewportLimit(std::max(1, controller->content()->height() / 2));
	_list->setKeyHandler([=](QKeyEvent *event) { return keyPress(event); });
	_list->setSelectionActions([=](uint64_t key) { mark(key); }, {});
	_list->setContextActions([=](not_null<Ui::PopupMenu*> menu, uint64_t key) {
		fillActions(menu, key);
	});
	box->setFocusCallback([=] {
		if (ModalKeysEnabled()) focusList();
		else _search->setFocusFast();
	});
	box->setCloseByEscape(false);
	box->setWidth(std::max(st::boxWidth, std::min(
		st::purpleMusicMaxWidth,
		controller->content()->width() - 2 * st::purpleMusicMargin)));
	box->setTitle(rpl::single(u"Music shared in this chat"_q));
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	box->addButton(rpl::single(u"Actions"_q), [=] { _list->showActions(); });
	_more = box->addLeftButton(tr::lng_purple_music_more(), [=] {
		if (_hasMore && !_loading && current()) {
			_limit += kPageSize;
			load();
		}
	});
	_more->hide();
	const auto player = Media::Player::instance();
	player->updatedNotifier() | rpl::on_next([=](const Media::Player::TrackState &state) {
		if (state.id.type() == AudioMsgId::Type::Song) updatePlayback();
	}, box->lifetime());
	player->stops(AudioMsgId::Type::Song) | rpl::on_next([=] {
		updatePlayback();
	}, box->lifetime());
	load();
}

bool ChatMusicBox::current() const {
	const auto controller = _controller.get();
	return controller && &controller->session() == _session.get()
		&& _session->account().maybeSession() == _session.get()
		&& !_session->account().loggingOut()
		&& !Core::App().passcodeLocked() && !controller->window().locked()
		&& controller->window().sessionController() == controller;
}

uint64_t ChatMusicBox::rowKey(FullMsgId id) const {
	return uint64_t(id.msg.bare) * 2 + (id.peer != _peer);
}

const SharedTrack *ChatMusicBox::track(uint64_t key) const {
	const auto it = std::ranges::find_if(_tracks, [=](const SharedTrack &track) {
		return track.row.documentId == key;
	});
	return (it == _tracks.end()) ? nullptr : &*it;
}

HistoryItem *ChatMusicBox::item(uint64_t key) const {
	if (!current()) return nullptr;
	const auto source = track(key);
	return source ? _session->data().message(source->message) : nullptr;
}

void ChatMusicBox::load() {
	_loading = true;
	setStatus(tr::lng_contacts_loading(tr::now));
	_viewer.destroy();
	SharedMediaMergedViewer(
		_session,
		SharedMediaMergedKey(
			SparseIdsMergedSlice::Key(
				_peer, _topic, _sublist, _migrated, ServerMaxMsgId - 1),
			Storage::SharedMediaType::MusicFile),
		_limit,
		0
	) | rpl::on_next([=](const SparseIdsMergedSlice &slice) {
		if (current() && slice.fullCount()) apply(slice);
	}, _viewer);
}

void ChatMusicBox::apply(const SparseIdsMergedSlice &slice) {
	auto tracks = std::vector<SharedTrack>();
	tracks.reserve(slice.size());
	for (auto i = slice.size(); i > 0;) {
		const auto id = slice[--i];
		const auto item = _session->data().message(id);
		const auto media = item ? item->media() : nullptr;
		const auto document = media ? media->document() : nullptr;
		if (!document || !document->isSong()) continue;
		const auto song = document->song();
		auto title = song ? song->title : QString();
		auto performer = song ? song->performer : QString();
		const auto filename = document->filename();
		if (title.trimmed().isEmpty()) title = filename;
		tracks.push_back({
			.message = id,
			.row = {
				.documentId = rowKey(id),
				.title = title,
				.performer = performer,
				.duration = Ui::FormatDurationText(document->duration() / 1000),
			},
			.searchText = title + u' ' + performer + u' ' + filename,
		});
	}
	const auto available = *slice.fullCount();
	if (!slice.size() && available && _loading) return;
	_loading = (slice.size() < std::min(_limit, available))
		&& !slice.skippedBefore();
	_hasMore = slice.skippedBefore().value_or(available - slice.size()) > 0;
	_more->setVisible(_hasMore);
	_more->setDisabled(_loading);
	_tracks = std::move(tracks);
	render();
}

void ChatMusicBox::render(std::optional<Anchor> anchor) {
	if (!anchor) anchor = Anchor{ _list->selectedDocument(), _list->scrollTop() };
	auto visible = std::vector<PlaylistSongListRow>();
	for (const auto &track : _tracks) {
		if (MatchExactSearch(_terms, track.searchText)) visible.push_back(track.row);
	}
	const auto empty = visible.empty();
	_list->setRows(std::move(visible), anchor->cursor, anchor->scrollTop);
	updatePlayback();
	setStatus(_loading ? tr::lng_contacts_loading(tr::now)
		: !empty ? QString()
		: !_terms.empty() ? tr::lng_purple_music_no_results(tr::now)
		: u"No music has been shared here."_q);
}

void ChatMusicBox::updatePlayback() {
	const auto state = Media::Player::instance()->getState(AudioMsgId::Type::Song);
	const auto document = state.id.audio();
	auto key = uint64_t(0);
	auto playback = MusicRowPlayback::Idle;
	if (document && &document->session() == _session.get()
		&& !Media::Player::IsStoppedOrStopping(state.state)) {
		const auto id = state.id.contextId();
		if (id.peer == _peer || (_migrated && id.peer == _migrated)) {
			key = rowKey(id);
			playback = Media::Player::IsPausedOrPausing(state.state)
				? MusicRowPlayback::Paused
				: (state.waitingForData || state.state == Media::Player::State::Starting)
				? MusicRowPlayback::Loading
				: MusicRowPlayback::Playing;
		}
	}
	_list->setMusicPlayback(key, playback);
}

void ChatMusicBox::filter() {
	const auto next = ExactSearchTerms(_search->getLastText());
	if (next == _terms) return;
	if (_terms.empty()) {
		_unfiltered = Anchor{ _list->selectedDocument(), _list->scrollTop() };
	}
	_terms = next;
	render(_terms.empty() ? base::take(_unfiltered) : std::nullopt);
}

void ChatMusicBox::focusList() {
	_list->focusSelected();
}

bool ChatMusicBox::eventFilter(QObject *object, QEvent *event) {
	if (object != _search->rawTextEdit() || event->type() != QEvent::KeyPress) return false;
	const auto key = static_cast<QKeyEvent*>(event);
	const auto layout = _search->textCursor().block().layout();
	if (layout && !layout->preeditAreaText().isEmpty()) return false;
	if (key->key() == Qt::Key_Escape && !key->modifiers()) {
		_search->setText(QString());
		filter();
		focusList();
	} else if (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up) {
		const auto down = key->key() == Qt::Key_Down;
		if (key->modifiers() == Qt::AltModifier) {
			_list->navigate(down ? TrackListAction::HalfDown : TrackListAction::HalfUp);
		} else if (!key->modifiers()) {
			_list->navigate(down ? TrackListAction::Down : TrackListAction::Up);
		} else {
			return false;
		}
	} else {
		return false;
	}
	return true;
}

bool ChatMusicBox::keyPress(QKeyEvent *event) {
	const auto key = ModalPhysicalKey(*event);
	const auto mods = event->modifiers() & ~Qt::KeypadModifier;
	if (key == Qt::Key_Escape && !mods) {
		if (_pendingG) _pendingG = false;
		else if (_marked.empty()) _box->closeBox();
		else {
			_marked.clear();
			_list->setChosenSongs({});
		}
		return true;
	}
	if (!ModalKeysEnabled()) return false;
	const auto shift = (mods == Qt::ShiftModifier);
	const auto motion = !mods && (key == Qt::Key_J || key == Qt::Key_K);
	if (motion) {
		if (AllowModalLayerKey(*event, _box, true)) {
			_pendingG = false;
			_list->navigate(key == Qt::Key_J ? TrackListAction::Down : TrackListAction::Up);
		}
		return true;
	}
	if ((!mods && (key == Qt::Key_G || key == Qt::Key_M || key == Qt::Key_Slash))
		|| (shift && key == Qt::Key_G)) {
		if (!AllowModalLayerKey(*event, _box)) return true;
		if (key == Qt::Key_G && !shift && !_pendingG) {
			_pendingG = true;
		} else {
			_pendingG = false;
			if (key == Qt::Key_G) {
				_list->navigate(shift ? TrackListAction::Last : TrackListAction::First);
			} else if (key == Qt::Key_M) {
				_list->navigate(TrackListAction::Mark);
			} else {
				_search->setFocusFast();
			}
		}
		return true;
	}
	_pendingG = false;
	return false;
}

void ChatMusicBox::play(uint64_t key) {
	const auto source = item(key);
	const auto media = source ? source->media() : nullptr;
	const auto document = media ? media->document() : nullptr;
	if (!document || !document->isSong() || !source->computeUnavailableReason().isEmpty()) {
		setStatus(tr::lng_purple_music_track_unavailable(tr::now));
		return;
	}
	const auto player = Media::Player::instance();
	const auto state = player->getState(AudioMsgId::Type::Song);
	const auto audio = AudioMsgId(document, source->fullId());
	if (state.id == audio && !Media::Player::IsStoppedOrStopping(state.state)) {
		if (Media::Player::IsPausedOrPausing(state.state)) player->play(AudioMsgId::Type::Song);
	} else {
		player->play(audio, Media::Player::PlaylistContext{ _topic, _sublist });
	}
	updatePlayback();
}

void ChatMusicBox::forward(uint64_t key) {
	if (!current()) return;
	const auto keys = _marked.contains(key) ? _marked : std::set<uint64_t>{ key };
	auto items = MessageIdsList();
	for (const auto &track : _tracks) {
		if (!keys.contains(track.row.documentId)) continue;
		if (!MatchExactSearch(_terms, track.searchText)) break;
		const auto source = item(track.row.documentId);
		if (!source || !source->allowsForward()) break;
		items.push_back(source->fullId());
	}
	if (items.size() != keys.size()) {
		setStatus(u"All marked tracks must be visible and available to forward."_q);
		return;
	}
	const auto picker = Window::ShowForwardMessagesBox(_controller.get(), std::move(items));
	if (!picker) setStatus(u"Could not open the forwarding picker."_q);
}

void ChatMusicBox::goTo(uint64_t key) {
	if (const auto source = item(key)) {
		const auto controller = _controller;
		_box->closeBox();
		if (const auto strong = controller.get()) {
			strong->showMessage(source, Window::SectionShow::Way::Forward);
		}
	} else {
		setStatus(tr::lng_purple_music_track_unavailable(tr::now));
	}
}

void ChatMusicBox::addToPlaylist(uint64_t key) {
	const auto source = item(key);
	const auto media = source ? source->media() : nullptr;
	const auto document = media ? media->document() : nullptr;
	if (!document || !CapturePlaylistSong(_session, { source->fullId(), document->id })) {
		setStatus(tr::lng_purple_music_track_unavailable(tr::now));
		return;
	}
	ShowPlaylistPicker(_controller.get(), { source->fullId(), document->id });
}

void ChatMusicBox::copyLink(uint64_t key) {
	const auto source = item(key);
	if (!source || !source->hasDirectLink() || IsAnchoredEphemeral(source)) {
		setStatus(u"No message link is available for this track."_q);
		return;
	}
	HistoryView::CopyPostLink(_controller.get(), source->fullId(), HistoryView::Context::History);
}

void ChatMusicBox::fillActions(not_null<Ui::PopupMenu*> menu, uint64_t key) {
	menu->addAction(u"Play"_q, [=] { play(key); });
	menu->addAction(tr::lng_context_forward_msg(tr::now), [=] { forward(key); });
	menu->addAction(tr::lng_context_to_msg(tr::now), [=] { goTo(key); });
	menu->addAction(tr::lng_purple_playlists_add(tr::now), [=] { addToPlaylist(key); });
	menu->addAction(tr::lng_context_copy_link(tr::now), [=] { copyLink(key); });
}

void ChatMusicBox::mark(uint64_t key) {
	if (!_marked.erase(key)) _marked.insert(key);
	_list->setChosenSongs(_marked);
}

void ChatMusicBox::setStatus(const QString &text) {
	_status->setText(text);
	_status->setVisible(!text.isEmpty());
}

} // namespace

void ShowChatMusicBox(
	not_null<Window::SessionController*> controller,
	not_null<Data::Thread*> thread) {
	const auto weak = base::make_weak(controller.get());
	const auto live = thread->migrateToOrMe();
	const auto topic = live->topicRootId();
	const auto sublist = live->monoforumPeerId();
	const auto previous = (!topic && !sublist) ? live->peer()->migrateFrom() : nullptr;
	const auto source = SparseIdsMergedSlice::Key(
		live->peer()->id,
		topic,
		sublist,
		previous ? previous->id : PeerId(),
		ServerMaxMsgId - 1);
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		if (const auto controller = weak.get()) {
			new ChatMusicBox(box, controller, source);
		}
	}));
}

} // namespace Purple

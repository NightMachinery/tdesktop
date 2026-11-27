#include "purple/purple_chat_music.h"

#include "base/random.h"
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
#include "purple/purple_list_filter_keys.h"
#include "purple/purple_media_seek.h"
#include "purple/purple_music_now_playing.h"
#include "purple/purple_music_section.h"
#include "purple/purple_music_widgets.h"
#include "purple/purple_playlist_player.h"
#include "purple/purple_playlist_song_list.h"
#include "purple/purple_playlists_box.h"
#include "purple/purple_playlists_service.h"
#include "rpl/variable.h"
#include "ui/layers/generic_box.h"
#include "ui/text/format_values.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/slide_wrap.h"
#include "window/window_controller.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"

#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_purple_music.h"
#include "styles/style_widgets.h"

#include <QtCore/QDateTime>
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

class ChatMusicBox final : public QObject, public ChatMusicKeyTarget {
public:
	ChatMusicBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		SparseIdsMergedSlice::Key source);

	[[nodiscard]] bool ownsKeys(
		not_null<Window::SessionController*> controller,
		not_null<QWidget*> focus) const;
	void navigate(TrackListAction action, int count) override;
	void act(MusicTrackAction action) override;
	void focusFilter() override;

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
	[[nodiscard]] std::vector<PlaylistMessage> selectedMessages(uint64_t key, bool visibleOnly = false);
	void load();
	void apply(const SparseIdsMergedSlice &slice);
	void render(std::optional<Anchor> anchor = {});
	void updatePlayback();
	void filter();
	void focusList();
	bool keyPress(QKeyEvent *event);
	void play(uint64_t key);
	bool playMarked(bool visibleOnly = false);
	void queue(uint64_t key);
	void forward(uint64_t key);
	void goTo(uint64_t key);
	void addToPlaylist(uint64_t key);
	void copyLink(uint64_t key);
	void fillActions(not_null<Ui::PopupMenu*> menu, uint64_t key);
	void mark(uint64_t key);
	void updateMarks();
	void setStatus(const QString &text);

	const not_null<Ui::GenericBox*> _box;
	const base::weak_ptr<Window::SessionController> _controller;
	const not_null<Main::Session*> _session;
	const PeerId _peer;
	const MsgId _topic;
	const PeerId _sublist;
	const PeerId _migrated;
	ChatMusicLayout _view;
	rpl::variable<QString> _markSummary;
	std::vector<SharedTrack> _tracks;
	std::set<uint64_t> _marked;
	std::optional<Anchor> _unfiltered;
	QStringList _terms;
	rpl::lifetime _viewer;
	int _limit = kPageSize;
	bool _loading = true;
	bool _hasMore = false;
	bool _tail = false;
	bool _closed = false;

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
, _migrated(source.migratedPeerId) {
	const auto keys = MusicEffectiveShortcuts();
	_view = AddChatMusicLayout(box, {
		.player = MakeMusicNowPlaying(box, _session, true, keys),
		.tiles = MusicTilesValue(),
		.marks = _markSummary.value(),
		.selection = {
			.play = [=] { playMarked(); },
			.queue = [=] { if (!_marked.empty()) queue(*_marked.begin()); },
			.add = [=] { if (!_marked.empty()) addToPlaylist(*_marked.begin()); },
			.forward = [=] { if (!_marked.empty()) forward(*_marked.begin()); },
			.clear = [=] { _marked.clear(); updateMarks(); },
		},
		.selectView = [](bool tiles) { if (tiles != MusicTiles()) ToggleMusicTiles(); },
		.play = [=](uint64_t key) { play(key); },
		.loadMore = [=] {
			if (_hasMore && !_loading && current()) {
				_limit += kPageSize;
				load();
			}
		},
	}, keys);
	_view.filter->rawTextEdit()->installEventFilter(this);
	box->installEventFilter(this);
	_view.filter->changes() | rpl::on_next([=] { filter(); }, box->lifetime());
	_view.tracks->setViewportLimit(std::max(1, controller->content()->height() / 2));
	_view.tracks->setKeyHandler([=](QKeyEvent *event) { return keyPress(event); });
	_view.tracks->setSelectionActions([=](uint64_t key) { mark(key); },
		[=] { return playMarked(); }, [=](std::vector<uint64_t> keys) {
			_marked.insert(keys.begin(), keys.end());
			updateMarks();
		});
	_view.tracks->setArtworkSession(_session);
	_view.tracks->setContextActions([=](not_null<Ui::PopupMenu*> menu, uint64_t key) {
		fillActions(menu, key);
	});
	box->setFocusCallback([=] {
		if (ModalKeysEnabled()) focusList();
		else focusFilter();
	});
	box->setCloseByEscape(false);
	box->boxClosing() | rpl::on_next([=] {
		_closed = true;
		_viewer.destroy();
	}, box->lifetime());
	controller->content()->widthValue() | rpl::on_next([=](int width) {
		box->setWidth(ChatMusicWidth(width));
	}, box->lifetime());
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
	return !_closed && controller && &controller->session() == _session.get()
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

std::vector<PlaylistMessage> ChatMusicBox::selectedMessages(uint64_t key, bool visibleOnly) {
	if (!current()) return {};
	const auto keys = _marked.contains(key) ? _marked : std::set<uint64_t>{ key };
	auto messages = std::vector<PlaylistMessage>();
	messages.reserve(keys.size());
	for (const auto &track : _tracks) {
		if (!keys.contains(track.row.documentId) || !MatchExactSearch(_terms, track.searchText)) {
			continue;
		}
		messages.push_back({ track.message, track.row.audioDocument });
	}
	if (messages.empty() || (!visibleOnly && messages.size() != keys.size())) {
		setStatus(u"Clear the filter or clear hidden marks first."_q);
		return {};
	}
	return messages;
}

void ChatMusicBox::load() {
	_loading = true;
	setStatus(tr::lng_contacts_loading(tr::now));
	_viewer.destroy();
	SharedMediaMergedViewer(
		_session,
		SharedMediaMergedKey(
			SparseIdsMergedSlice::Key(
				_peer, _topic, _sublist, _migrated,
				_tail ? (_migrated ? 1 - ServerMaxMsgId : MsgId(1)) : ServerMaxMsgId - 1),
			Storage::SharedMediaType::MusicFile),
		_tail ? 0 : _limit,
		_tail ? _limit : 0
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
				.message = id,
				.audioDocument = document->id,
			},
			.searchText = title + u' ' + performer + u' ' + filename,
		});
	}
	const auto available = *slice.fullCount();
	if (!slice.size() && available && _loading) return;
	_loading = (slice.size() < std::min(_limit, available))
		&& !slice.skippedBefore();
	_hasMore = !_tail && slice.skippedBefore().value_or(available - slice.size()) > 0;
	_view.loadMore->setVisible(_hasMore);
	_view.loadMore->setDisabled(_loading);
	_tracks = std::move(tracks);
	render();
	if (_tail) _view.tracks->navigate(TrackListAction::Last);
}

void ChatMusicBox::render(std::optional<Anchor> anchor) {
	if (!anchor) anchor = Anchor{ _view.tracks->selectedDocument(), _view.tracks->scrollTop() };
	auto visible = std::vector<PlaylistSongListRow>();
	for (const auto &track : _tracks) {
		if (MatchExactSearch(_terms, track.searchText)) visible.push_back(track.row);
	}
	const auto empty = visible.empty();
	_view.tracks->setRows(std::move(visible), anchor->cursor, anchor->scrollTop);
	updateMarks();
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
	_view.tracks->setMusicPlayback(key, playback);
}

void ChatMusicBox::filter() {
	const auto next = ExactSearchTerms(_view.filter->getLastText());
	if (next == _terms) return;
	if (_terms.empty()) {
		_unfiltered = Anchor{ _view.tracks->selectedDocument(), _view.tracks->scrollTop() };
	}
	_terms = next;
	render(_terms.empty() ? base::take(_unfiltered) : std::nullopt);
}

void ChatMusicBox::focusList() {
	_view.tracks->focusSelected();
}

bool ChatMusicBox::eventFilter(QObject *object, QEvent *event) {
	if (object == _box.get()) {
		return event->type() == QEvent::KeyPress
			&& keyPress(static_cast<QKeyEvent*>(event));
	}
	if (!_closed && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)) {
		_view.tracks->setFilterFocused(event->type() == QEvent::FocusIn);
	}
	if (event->type() != QEvent::KeyPress) return false;
	const auto key = static_cast<QKeyEvent*>(event);
	const auto layout = _view.filter->textCursor().block().layout();
	if (layout && !layout->preeditAreaText().isEmpty()) return false;
	if (key->key() == Qt::Key_Escape && !key->modifiers()) {
		_view.filter->setText(QString());
		filter();
		focusList();
	} else if (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up) {
		const auto down = key->key() == Qt::Key_Down;
		if (key->modifiers() == Qt::AltModifier) {
			_view.tracks->navigate(down ? TrackListAction::HalfDown : TrackListAction::HalfUp);
		} else if (!key->modifiers()) {
			_view.tracks->navigate(down ? TrackListAction::Down : TrackListAction::Up);
		} else {
			return false;
		}
	} else {
		const auto mods = key->modifiers() & ~Qt::KeypadModifier;
		if (!mods && ListFilter::Submit(key->key())) {
			if (!key->isAutoRepeat()) _view.tracks->navigate(TrackListAction::Play);
		} else if (!mods && (key->key() == Qt::Key_PageUp || key->key() == Qt::Key_PageDown)) {
			_view.tracks->navigate(key->key() == Qt::Key_PageUp
				? TrackListAction::PageUp : TrackListAction::PageDown);
		} else if (ModalKeysEnabled() && ListFilter::Exit(key->key(), mods)) {
			focusList();
		} else if (ModalKeysEnabled() && mods == ListFilter::kControl
			&& (key->key() == Qt::Key_J || key->key() == Qt::Key_K)) {
			_view.tracks->navigate(key->key() == Qt::Key_J
				? TrackListAction::Down : TrackListAction::Up);
		} else {
			return false;
		}
	}
	return true;
}

bool ChatMusicBox::keyPress(QKeyEvent *event) {
	if (event->modifiers() & ~Qt::KeypadModifier) return false;
	const auto key = event->key();
	if (key == Qt::Key_Escape) {
		if (_view.tracks->cancelRangeMark()) return true;
		if (_marked.empty()) _box->closeBox();
		else {
			_marked.clear();
			updateMarks();
		}
		return true;
	}
	if (key == Qt::Key_Home || key == Qt::Key_End) {
		navigate(key == Qt::Key_Home ? TrackListAction::First : TrackListAction::Last, 1);
		return true;
	}
	if (ModalKeysEnabled() && !MusicTiles() && current()
		&& (key == Qt::Key_Left || key == Qt::Key_Right)) {
		const auto current = Media::Player::instance()->current(AudioMsgId::Type::Song);
		if (current.audio() && &current.audio()->session() == _session.get()) {
			SeekBy(AudioMsgId::Type::Song, key == Qt::Key_Left ? -1 : 1);
		}
		return true;
	}
	return false;
}

bool ChatMusicBox::ownsKeys(
		not_null<Window::SessionController*> controller,
		not_null<QWidget*> focus) const {
	return controller.get() == _controller.get() && current()
		&& (focus == _box.get() || _view.tracks->isAncestorOf(focus));
}

void ChatMusicBox::navigate(TrackListAction action, int count) {
	if (action == TrackListAction::First || action == TrackListAction::Last) {
		const auto tail = action == TrackListAction::Last;
		if (_tail != tail && (_hasMore || _tail)) {
			_tail = tail;
			_limit = kPageSize;
			load();
			return;
		}
	}
	_view.tracks->navigate(action, count);
}

void ChatMusicBox::act(MusicTrackAction action) {
	const auto track = _view.tracks->selectedDocument();
	switch (action) {
	case MusicTrackAction::Forward: forward(track); break;
	case MusicTrackAction::GoTo: goTo(track); break;
	case MusicTrackAction::AddToPlaylist: addToPlaylist(track); break;
	case MusicTrackAction::Queue: queue(track); break;
	case MusicTrackAction::CopyMessageLink: copyLink(track); break;
	case MusicTrackAction::Menu: _view.tracks->showActions(); break;
	}
}

void ChatMusicBox::focusFilter() {
	_view.filter->setFocusFast();
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

bool ChatMusicBox::playMarked(bool visibleOnly) {
	if (_marked.empty()) return false;
	const auto messages = selectedMessages(*_marked.begin(), visibleOnly);
	if (messages.empty()) return true;
	if (messages.size() == 1) {
		play(rowKey(messages.front().id));
		return true;
	}
	auto songs = std::vector<PlaylistAddSongInput>();
	for (const auto &message : messages) {
		const auto song = CapturePlaylistSong(_session, message);
		if (!song) {
			setStatus(tr::lng_purple_music_track_unavailable(tr::now));
			return true;
		}
		songs.push_back(*song);
	}
	auto entropy = QByteArray(kPlaylistIdEntropyBytes, char(0));
	base::RandomFill(entropy.data(), entropy.size());
	const auto weak = QPointer(this);
	EditAccountPlaylists(_session, {
		.kind = PlaylistAccountEditKind::CreateHyper,
		.entropy = std::move(entropy),
		.songs = std::move(songs),
		.nowMs = uint64_t(QDateTime::currentMSecsSinceEpoch()),
	}, [weak, document = messages.front().documentId](PlaylistAccountResult result) {
		if (!weak || !weak->current()) return;
		if (result && result.edit) {
			PlayAccountPlaylist(weak->_session, result.edit->playlistId, document);
		} else {
			weak->setStatus(tr::lng_purple_playlists_write_failed(tr::now));
		}
	});
	return true;
}

void ChatMusicBox::queue(uint64_t key) {
	const auto messages = selectedMessages(key);
	if (messages.empty()) return;
	setStatus(AppendAccountMusicQueue(_session, messages)
		? u"Added to the playback queue."_q
		: tr::lng_purple_music_track_unavailable(tr::now));
}

void ChatMusicBox::forward(uint64_t key) {
	const auto messages = selectedMessages(key);
	if (messages.empty()) return;
	auto items = MessageIdsList();
	for (const auto &message : messages) {
		const auto source = _session->data().message(message.id);
		if (!source || !source->allowsForward()) break;
		items.push_back(source->fullId());
	}
	if (items.size() != messages.size()) {
		setStatus(u"All marked tracks must be visible and available to forward."_q);
		return;
	}
	const auto picker = Window::ShowForwardMessagesBox(_controller.get(), std::move(items));
	if (!picker) setStatus(u"Could not open the forwarding picker."_q);
}

void ChatMusicBox::goTo(uint64_t key) {
	if (_marked.size() > 1 && _marked.contains(key)) {
		setStatus(u"Choose one track for this action."_q);
		return;
	}
	if (const auto source = item(key)) {
		const auto controller = _controller;
		const auto id = source->fullId();
		_box->closeBox();
		if (const auto strong = controller.get()) {
			if (const auto live = strong->session().data().message(id)) {
				strong->showMessage(live, Window::SectionShow::Way::Forward);
			}
		}
	} else {
		setStatus(tr::lng_purple_music_track_unavailable(tr::now));
	}
}

void ChatMusicBox::addToPlaylist(uint64_t key) {
	auto messages = selectedMessages(key);
	if (messages.empty()) return;
	if (!std::ranges::all_of(messages, [=](const PlaylistMessage &message) {
		return bool(CapturePlaylistSong(_session, message));
	})) {
		setStatus(tr::lng_purple_music_track_unavailable(tr::now));
		return;
	}
	ShowPlaylistPicker(_controller.get(), std::move(messages));
}

void ChatMusicBox::copyLink(uint64_t key) {
	if (_marked.size() > 1 && _marked.contains(key)) {
		setStatus(u"Choose one track for this action."_q);
		return;
	}
	const auto source = item(key);
	if (!source || !source->hasDirectLink() || IsAnchoredEphemeral(source)) {
		setStatus(u"No message link is available for this track."_q);
		return;
	}
	HistoryView::CopyPostLink(_controller.get(), source->fullId(), HistoryView::Context::History);
}

void ChatMusicBox::fillActions(not_null<Ui::PopupMenu*> menu, uint64_t key) {
	menu->addAction(u"Play"_q, [=] { play(key); });
	menu->addAction(u"Add to queue"_q, [=] { queue(key); });
	menu->addAction(tr::lng_context_forward_msg(tr::now), [=] { forward(key); });
	menu->addAction(tr::lng_context_to_msg(tr::now), [=] { goTo(key); });
	menu->addAction(tr::lng_purple_playlists_add(tr::now), [=] { addToPlaylist(key); });
	menu->addAction(tr::lng_context_copy_link(tr::now), [=] { copyLink(key); });
	if (!_marked.empty()) {
		menu->addAction(u"Play visible marks"_q, [=] { playMarked(true); });
		menu->addAction(u"Clear marks"_q, [=] {
			_marked.clear();
			updateMarks();
		});
		menu->addAction(u"Clear hidden marks"_q, [=] {
			std::erase_if(_marked, [=](uint64_t id) {
				const auto row = track(id);
				return !row || !MatchExactSearch(_terms, row->searchText);
			});
			updateMarks();
		});
	}
}

void ChatMusicBox::mark(uint64_t key) {
	if (!_marked.erase(key)) _marked.insert(key);
	updateMarks();
}

void ChatMusicBox::updateMarks() {
	_view.tracks->setChosenSongs(_marked);
	const auto hidden = _marked.size() - std::ranges::count_if(_tracks, [=](const SharedTrack &track) {
		return _marked.contains(track.row.documentId) && MatchExactSearch(_terms, track.searchText);
	});
	_markSummary = hidden
		? u"%1 marked · %2 hidden"_q.arg(_marked.size()).arg(hidden)
		: _marked.empty() ? QString() : u"%1 marked"_q.arg(_marked.size());
}

void ChatMusicBox::setStatus(const QString &text) {
	_view.status->setText(text);
	_view.status->setVisible(!text.isEmpty());
}

} // namespace

ChatMusicKeyTarget *FindChatMusicKeyTarget(
		not_null<Window::SessionController*> controller,
		not_null<QWidget*> focus) {
	for (auto widget = focus.get(); widget; widget = widget->parentWidget()) {
		if (const auto box = dynamic_cast<Ui::GenericBox*>(widget)) {
			for (const auto child : box->children()) {
				const auto music = dynamic_cast<ChatMusicBox*>(child);
				if (music && music->ownsKeys(controller, focus)) return music;
			}
			return nullptr;
		}
	}
	return nullptr;
}

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

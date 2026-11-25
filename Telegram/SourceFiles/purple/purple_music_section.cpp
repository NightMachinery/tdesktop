#include "purple/purple_music_section.h"

#include "apiwrap.h"
#include "base/random.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "data/data_chat_filters.h"
#include "core/core_settings.h"
#include "mainwidget.h"
#include "data/data_document.h"
#include "data/data_folder.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "history/view/history_view_context_menu.h"
#include "history/view/history_view_element.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "media/audio/media_audio.h"
#include "media/player/media_player_instance.h"
#include "mtproto/facade.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/sender.h"
#include "purple/purple_config.h"
#include "purple/purple_keys.h"
#include "purple/purple_notices.h"
#include "purple/purple_copy.h"
#include "purple/purple_music_bookmark.h"
#include "purple/purple_music_diagnostics.h"
#include "purple/purple_music_index_service.h"
#include "purple/purple_music_now_playing.h"
#include "purple/purple_music_widgets.h"
#include "purple/purple_music_source_roster.h"
#include "purple/purple_playlist_player.h"
#include "purple/purple_playlist_song_list.h"
#include "purple/purple_playlists_box.h"
#include "purple/purple_playlists_menu.h"
#include "purple/purple_playlists_resolver.h"
#include "purple/purple_playlists_service.h"
#include "rpl/variable.h"
#include "settings/settings_common.h"
#include "storage/storage_account.h"
#include "ui/boxes/confirm_box.h"
#include "ui/widgets/popup_menu.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/layers/generic_box.h"
#include "ui/text/format_values.h"
#include "ui/toast/toast.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/section_memento.h"
#include "window/section_widget.h"
#include "window/window_controller.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"

#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_purple_music.h"
#include "styles/style_widgets.h"
#include "styles/style_window.h"
#include "styles/style_window_main_menu.h"

#include <QtCore/QDateTime>
#include <QtCore/QVariant>
#include <QtGui/QClipboard>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include "ui/painter.h"
#include <QtGui/QTextBlock>
#include <QtGui/QTextLayout>
#include <QtWidgets/QApplication>
#include <QtWidgets/QTextEdit>
#include <deque>
#include <set>

namespace Purple {
namespace {

constexpr auto kHideChatsPref = "purple.music.show-chats";
constexpr auto kScanRetryPref = "purple.music.scan-retry";
constexpr auto kSearchPageSize = 100;
constexpr auto kRefreshDelay = crl::time(180);
constexpr auto kConnectionPollDelay = crl::time(1500);

bool IsOfflineState(const Main::Session &session) {
	const auto state = session.account().mtp().dcstate();
	return state == MTP::ConnectingState
		|| state == MTP::DisconnectedState
		|| state < 0;
}

rpl::event_stream<Main::Session*> MusicFolderChanges;
rpl::event_stream<bool> MusicHideChatsChanges;

QString MusicBookmarkOrgLink(const MusicBookmark &bookmark) {
	auto label = bookmark.title;
	if (!bookmark.performer.isEmpty()) {
		if (!label.isEmpty()) label += u" · "_q;
		label += bookmark.performer;
	}
	for (auto &ch : label) {
		if (ch.isSpace() || ch.category() == QChar::Other_Control) ch = u' ';
		else if (ch == u'[') ch = u'(';
		else if (ch == u']') ch = u')';
	}
	label = label.simplified();
	const auto target = u"[[purpleaudio:"_q + bookmark.token + u']';
	return label.isEmpty() ? target + u']' : target + u'[' + label + u"]]"_q;
}

bool MusicWindowCurrent(not_null<Window::SessionController*> controller) {
	const auto &session = controller->session();
	return !Core::App().passcodeLocked()
		&& !controller->window().locked()
		&& !session.account().loggingOut()
		&& session.account().maybeSession() == &session
		&& controller->window().sessionController() == controller;
}

struct Scan {
	MusicSourceCursor saved;
	qint64 offset = 0;
	qint64 newest = 0;
	bool head = false;
};

struct ScanSchedule {
	MusicScanPacer pacer;
	QPointer<QWidget> owner;
	quint64 retryPeer = 0;
};

std::shared_ptr<ScanSchedule> ScanScheduleFor(Main::Session &session) {
	static auto schedules = std::map<Main::Session*, std::shared_ptr<ScanSchedule>>();
	if (const auto i = schedules.find(&session); i != schedules.end()) {
		return i->second;
	}
	auto result = std::make_shared<ScanSchedule>();
	const auto saved = session.local().readPref<QString>(kScanRetryPref);
	const auto until = saved.section(u' ', 0, 0).toLongLong();
	result->pacer.restore(crl::now(), until - QDateTime::currentMSecsSinceEpoch());
	result->retryPeer = saved.section(u' ', 1, 1).toULongLong();
	schedules.emplace(&session, result);
	session.lifetime().add([key = &session] { schedules.erase(key); });
	return result;
}

class MusicSection final : public Window::SectionWidget {
public:
	MusicSection(QWidget *parent, not_null<Window::SessionController*> controller);

	bool showInternal(
		not_null<Window::SectionMemento*> memento,
		const Window::SectionShow &params) override;
	std::shared_ptr<Window::SectionMemento> createMemento() override;
	QRect floatPlayerAvailableRect() override;
	bool floatPlayerHandleWheelEvent(QEvent *) override;
	[[nodiscard]] bool allowsModalKeys(QWidget *focus) const;
	[[nodiscard]] bool searchContains(QWidget *widget) const;
	void focusSearch(bool search);
	void navigate(TrackListAction action, int repeat);
	void act(MusicTrackAction action, uint64_t key = 0);

private:
	enum class CatalogRead {
		Reload,
		Append,
		Scan,
		Tail,
	};

	void doSetInnerFocus() override;
	void keyPressEvent(QKeyEvent *event) override;
	void paintEvent(QPaintEvent *event) override;
	[[nodiscard]] bool current() const;
	[[nodiscard]] FilterId folder() const;
	[[nodiscard]] bool inRoster(quint64 peer) const;
	[[nodiscard]] bool rosterCurrent() const;
	void reset(bool keepRows = false);
	void refresh(bool force = false);
	void scan();
	void advance(Scan scan, const MTPmessages_Messages &result);
	void addBackfill(MusicSourceCursor cursor);
	void read(CatalogRead mode);
	void render();
	void updateListHeight();
	void setStatus(const QString &text);
	void updateStatus();
	void pollConnection();
	void requestMissingDialogs();
	void showMenu(not_null<Ui::IconButton*> button);
	void toggleTrack(quint64 documentId);
	void markTracks(std::vector<uint64_t> ids);
	void updateSelection();
	void updatePlayback();
	bool playSelection(bool visibleOnly = false);
	void saveSelection(const std::vector<MusicIndexEntry> &tracks, bool visibleOnly);
	[[nodiscard]] PlaylistResolveSong resolveSong(const MusicIndexEntry &entry) const;
	[[nodiscard]] std::optional<PlaylistAddSongInput> captureTrack(
		const MusicIndexEntry &entry) const;
	void play(quint64 documentId);
	void copyOrgLink(quint64 documentId);
	void fillTrackActions(not_null<Ui::PopupMenu*> menu, uint64_t key);
	void performTrackAction(MusicTrackAction action, const std::vector<PlaylistMessage> &messages);
	void playResolved(const MusicIndexEntry &entry);

	not_null<Ui::VerticalLayout*> _content;
	not_null<Ui::InputField*> _search;
	not_null<Ui::SlideWrap<Ui::FlatLabel>*> _status;
	Ui::SlideWrap<Ui::RoundButton> *_more = nullptr;
	Ui::RpWidget *_playChosen = nullptr;
	QPointer<PlaylistSongList> _list;
	base::unique_qptr<Ui::PopupMenu> _menu;
	std::shared_ptr<ScanSchedule> _schedule;
	MTP::Sender _api;
	base::Timer _refreshTimer;
	base::Timer _readTimer;
	base::Timer _connectionTimer;
	base::Timer _scanTimer;
	mtpRequestId _request = 0;
	FilterId _folder = 0;
	QString _folderName;
	std::optional<MusicIndexScope> _scope;
	std::vector<quint64> _peers;
	std::deque<Scan> _pending;
	std::vector<MusicIndexEntry> _rows;
	std::vector<MusicIndexEntry> _selection;
	rpl::variable<int> _selectionCount = 0;
	rpl::variable<int> _hiddenSelectionCount = 0;
	QString _displayedSearch;
	std::optional<MusicIndexCursor> _cursor;
	uint64_t _unfilteredCursor = 0;
	int _unfilteredTop = 0;
	bool _restoreUnfiltered = false;
	std::unique_ptr<DesktopPlaylistResolver> _resolver;
	std::unique_ptr<DesktopPlaylistResolver> _bookmarkResolver;
	std::unique_ptr<DesktopPlaylistResolver> _actionResolver;
	uint64_t _route = 0;
	uint64_t _query = 0;
	quint64 _loadingDocument = 0;
	bool _moreAvailable = false;
	bool _tail = false;
	bool _reading = false;
	bool _snapshotHeld = false;
	bool _scanIncomplete = false;
	bool _configuring = false;
	bool _savingSelection = false;
	bool _waitingMainDialogs = false;
	bool _waitingArchiveDialogs = false;
	bool _waitingForScanner = false;
	bool _savingPage = false;
	bool _offline = false;
	int _loadedMainDialogs = 0;
	int _loadedArchiveDialogs = 0;
	int _dialogStallTicks = 0;
	int _chatsToScan = 0;
	std::set<quint64> _completedPeers;
	std::set<quint64> _failedPeers;
	int _serverSearchErrors = 0;
	QString _lastServerError;
	int _localWriteErrors = 0;
	QString _indexError;
	int _messagesInspected = 0;
	int _tracksIndexedInScan = 0;
	int _skippedProtected = 0;
	int _skippedUnsupported = 0;
	int _skippedInvalid = 0;
	qint64 _totalIndexedTracks = 0;
	QString _transientNotice;

};

class MusicMemento final : public Window::SectionMemento {
public:
	object_ptr<Window::SectionWidget> createWidget(
			QWidget *parent,
			not_null<Window::SessionController*> controller,
			Window::Column column,
			const QRect &geometry) override {
		if (column == Window::Column::Third) return nullptr;
		auto result = object_ptr<MusicSection>(parent, controller);
		result->setGeometry(geometry);
		return result;
	}

};

MusicSection::MusicSection(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Window::SectionWidget(parent, controller)
, _content(Ui::CreateChild<Ui::VerticalLayout>(this))
, _search(_content->add(MakeMusicFilter(_content, MusicEffectiveShortcuts().filter),
	st::purpleMusicSearchMargin))
, _status(_content->add(object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
	_content,
	object_ptr<Ui::FlatLabel>(_content, QString(), st::boxLabel),
	st::purpleMusicStatusMargin)))
, _schedule(ScanScheduleFor(session()))
, _api(&session().mtp())
, _refreshTimer([=] { refresh(); })
, _readTimer([=] {
	read(_search->getLastText().trimmed() == _displayedSearch
		? CatalogRead::Scan : CatalogRead::Reload);
})
, _connectionTimer([=] { pollConnection(); })
, _scanTimer([=] { scan(); }) {
	setFocusPolicy(Qt::StrongFocus);
	const auto keys = MusicEffectiveShortcuts();
	_content->insert(0, MakeMusicHeader(
		_content,
		keys.playlists,
		[=] { controller->showBackFromStack(); },
		[=] { if (current()) ShowPlaylistsBox(controller); },
		[=](not_null<Ui::IconButton*> button) { showMenu(button); }
	), st::purpleMusicListMargin);
	_content->insert(1, MakeMusicNowPlaying(_content, &session(), true, keys), st::purpleMusicNowMargin);
	_content->insert(2, MakeMusicViewControl(_content, MusicTilesValue(), [](bool tiles) {
		if (tiles != MusicTiles()) ToggleMusicTiles();
	}, keys.view), st::purpleMusicActionMargin);

	const auto addAction = [=](
			rpl::producer<QString> text,
			const style::RoundButton &buttonStyle,
			style::align align,
			Fn<void()> callback) {
		const auto wrap = _content->add(object_ptr<Ui::SlideWrap<Ui::RoundButton>>(
			_content,
			object_ptr<Ui::RoundButton>(_content, std::move(text), buttonStyle),
			st::purpleMusicActionMargin), align);
		wrap->entity()->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
		wrap->entity()->setClickedCallback(std::move(callback));
		return wrap;
	};
	const auto folder = addAction(tr::lng_purple_music_source(),
		st::defaultLightButton, style::al_left,
		[=] { ChooseMusicSourceFolder(controller); });
	const auto updateFolderButton = [=] {
		const auto selected = this->folder();
		const auto &filters = session().data().chatsFilters();
		const auto exists = selected > 0 && !Data::IsPurpleView(selected)
			&& (!filters.loaded()
				|| std::ranges::find(filters.list(), selected, &Data::ChatFilter::id)
					!= filters.list().end());
		folder->toggle(!exists, anim::type::instant);
	};
	auto changes = MusicFolderChanges.events()
		| rpl::filter([=](Main::Session *session) { return session == &this->session(); })
		| rpl::to_empty;
	rpl::merge(session().data().chatsFilters().changed(), std::move(changes))
		| rpl::on_next(updateFolderButton, lifetime());
	updateFolderButton();
	_list = _content->add(object_ptr<PlaylistSongList>(
		_content, std::vector<PlaylistSongListRow>(), 0, 0,
		[=](uint64_t id) { play(id); },
		Fn<void(std::vector<uint64_t>)>(), PlaylistSongList::Mode::Library),
		st::purpleMusicListMargin);
	_list->setArtworkSession(&session());
	MusicTilesValue() | rpl::on_next([=](bool tiles) {
		_list->setTiles(tiles);
	}, lifetime());
	_list->setSelectionActions([=](uint64_t id) { toggleTrack(id); },
		[=] { return playSelection(); },
		[=](std::vector<uint64_t> ids) { markTracks(std::move(ids)); });
	_list->setKeyHandler([=](QKeyEvent *event) {
		if (event->modifiers() & ~Qt::KeypadModifier) return false;
		const auto key = event->key();
		if (key != Qt::Key_Home && key != Qt::Key_End) return false;
		navigate(key == Qt::Key_Home ? TrackListAction::First : TrackListAction::Last, 1);
		return true;
	});
	_list->setCopyOrgLinkAction(crl::guard(this, [=](uint64_t id) { copyOrgLink(id); }));
	_list->setContextActions([=](not_null<Ui::PopupMenu*> menu, uint64_t id) {
		fillTrackActions(menu, id);
	});
	_list->navigationEvents() | rpl::on_next([=] {
		if (_list->selectedDocument()) _snapshotHeld = true;
	}, lifetime());
	_more = addAction(tr::lng_purple_music_more(),
		st::defaultLightButton, style::al_right, [=] { read(CatalogRead::Append); });
	auto summary = rpl::combine(
		_selectionCount.value(),
		_hiddenSelectionCount.value(),
		[](int count, int hidden) {
			return hidden ? u"%1 marked · %2 hidden"_q.arg(count).arg(hidden)
				: count ? u"%1 marked"_q.arg(count) : QString();
		});
	_playChosen = _content->add(MakeMusicSelectionBar(_content, std::move(summary), {
		.play = [=] { playSelection(); },
		.queue = [=] { if (!_selection.empty()) act(MusicTrackAction::Queue, _selection.front().document); },
		.add = [=] { if (!_selection.empty()) act(MusicTrackAction::AddToPlaylist, _selection.front().document); },
		.forward = [=] { if (!_selection.empty()) act(MusicTrackAction::Forward, _selection.front().document); },
		.more = [=] { _list->showActions(); },
		.clear = [=] { _selection.clear(); updateSelection(); },
	}, keys), st::purpleMusicActionMargin);
	_more->hide(anim::type::instant);
	_status->hide(anim::type::instant);
	widthValue() | rpl::on_next([=](int width) {
		const auto contentWidth = std::min(width, st::purpleMusicMaxWidth);
		_content->resizeToWidth(contentWidth);
		_content->move((width - contentWidth) / 2, 0);
	}, lifetime());
	heightValue() | rpl::on_next([=] { updateListHeight(); }, lifetime());
	_content->heightValue() | rpl::on_next([=] { updateListHeight(); }, lifetime());
	_search->changes() | rpl::on_next([=] {
		const auto query = _search->getLastText().trimmed();
		if (_displayedSearch.isEmpty() && !query.isEmpty() && !_restoreUnfiltered) {
			_unfilteredCursor = _list->selectedDocument();
			_unfilteredTop = _list->scrollTop();
			_restoreUnfiltered = true;
		}
		++_query;
		_reading = false;
		_transientNotice.clear();
		if (query != _displayedSearch) {
			_more->hide(anim::type::instant);
			_readTimer.callOnce(kRefreshDelay);
		} else {
			_more->toggle(_moreAvailable, anim::type::instant);
			if (query.isEmpty()) _restoreUnfiltered = false;
		}
		updateStatus();
	}, lifetime());
	_search->submits() | rpl::on_next([=] {
		AcceptMusicFilter(controller->window().widget());
		_list->focusSelected();
	}, lifetime());
	const auto &data = session().data();
	data.chatsFilters().changed()
		| rpl::on_next([=] { _refreshTimer.callOnce(kRefreshDelay); }, lifetime());
	data.chatsListChanges() | rpl::on_next([=](auto) {
		_refreshTimer.callOnce(kRefreshDelay);
	}, lifetime());
	data.chatsListLoadedEvents() | rpl::on_next([=](auto) {
		_refreshTimer.callOnce(kRefreshDelay);
	}, lifetime());
	MusicFolderChanges.events() | rpl::on_next([=](Main::Session *session) {
		if (session == &this->session()) refresh();
	}, lifetime());
	MusicIndexArrivals(&session()) | rpl::on_next([=] {
		if (_scope && !_snapshotHeld && !_readTimer.isActive()) {
			_readTimer.callOnce(kRefreshDelay);
		}
	}, lifetime());
	const auto player = Media::Player::instance();
	player->trackChanged() | rpl::on_next([=](AudioMsgId::Type type) {
		if (type != AudioMsgId::Type::Song) return;
		if (base::take(_loadingDocument)) _resolver.reset();
		updatePlayback();
	}, lifetime());
	player->updatedNotifier() | rpl::on_next([=](const Media::Player::TrackState &state) {
		if (state.id.type() == AudioMsgId::Type::Song) updatePlayback();
	}, lifetime());
	player->stops(AudioMsgId::Type::Song) | rpl::on_next([=] {
		updatePlayback();
	}, lifetime());
	Core::App().passcodeLockChanges() | rpl::on_next([=](bool locked) {
		if (locked) reset();
		else refresh(true);
	}, lifetime());
	session().data().chatsFilters().load();
	refresh();
}

bool MusicSection::showInternal(
		not_null<Window::SectionMemento*> memento,
		const Window::SectionShow &) {
	if (!dynamic_cast<MusicMemento*>(memento.get())) return false;
	refresh();
	doSetInnerFocus();
	_search->selectAll();
	return true;
}

std::shared_ptr<Window::SectionMemento> MusicSection::createMemento() {
	return std::make_shared<MusicMemento>();
}

void MusicSection::doSetInnerFocus() {
	if (ModalKeysInNormal(controller()->window().widget())) {
		if (_list) _list->focusSelected();
		else setFocus();
	} else {
		_search->setFocusFast();
	}
}

void MusicSection::keyPressEvent(QKeyEvent *event) {
	if (!event->modifiers() && !isHidden() && current()) {
		if (_list && event->key() == Qt::Key_Down
			&& _search->rawTextEdit()->hasFocus()
			&& !_reading
			&& _displayedSearch == _search->getLastText().trimmed()
			&& _list->selectedDocument()
			&& rosterCurrent()) {
			const auto layout = _search->textCursor().block().layout();
			if (!layout || layout->preeditAreaText().isEmpty()) {
				event->accept();
				_list->focusSelected();
				return;
			}
		}
		if (event->key() == Qt::Key_Escape
			&& ModalKeysInNormal(controller()->window().widget())) {
			event->accept();
			if (_list && _list->cancelRangeMark()) {
				return;
			} else if (!_selection.empty()) {
				_selection.clear();
				updateSelection();
			} else {
				controller()->showBackFromStack();
			}
			return;
		}
	}
	Window::SectionWidget::keyPressEvent(event);
}

QRect MusicSection::floatPlayerAvailableRect() {
	return QRect(mapToGlobal(QPoint()), size());
}

bool MusicSection::floatPlayerHandleWheelEvent(QEvent *) {
	return false;
}

void MusicSection::paintEvent(QPaintEvent *event) {
	if (animatingShow()) {
		Window::SectionWidget::paintEvent(event);
		return;
	}
	auto painter = QPainter(this);
	painter.fillRect(event->rect(), st::boxBg->c);
}

void MusicSection::showMenu(not_null<Ui::IconButton*> button) {
	if (!current()) return;
	_menu = base::make_unique_q<Ui::PopupMenu>(this, st::defaultPopupMenu);
	_menu->addAction(tr::lng_purple_music_source(tr::now), [=] {
		ChooseMusicSourceFolder(controller());
	});
	_menu->addAction(tr::lng_purple_music_refresh(tr::now), [=] { refresh(true); });
	_menu->popup(button->mapToGlobal(QPoint(0, button->height())));
}

bool MusicSection::allowsModalKeys(QWidget *focus) const {
	if (!current() || !isVisible() || (focus != this && !isAncestorOf(focus))) return false;
	const auto layout = _search->textCursor().block().layout();
	return (!_search->rawTextEdit()->hasFocus()
		|| !layout
		|| layout->preeditAreaText().isEmpty());
}

bool MusicSection::searchContains(QWidget *widget) const {
	return widget && (widget == _search.get() || _search->isAncestorOf(widget));
}

void MusicSection::focusSearch(bool search) {
	if (!current() || !isVisible()) return;
	if (search) {
		_search->setFocusFast();
	} else if (searchContains(QApplication::focusWidget())) {
		_search->setText(QString());
		if (_list) _list->focusSelected();
		else setFocus();
	}
}

void MusicSection::navigate(TrackListAction action, int repeat) {
	if (!_list) return;
	if (!_reading && _tail && action == TrackListAction::First) {
		read(CatalogRead::Reload);
		return;
	} else if (!_reading && _moreAvailable && action == TrackListAction::Last) {
		read(CatalogRead::Tail);
		return;
	}
	_snapshotHeld = true;
	_list->navigate(action, repeat);
}

bool MusicSection::current() const {
	return MusicWindowCurrent(controller());
}

FilterId MusicSection::folder() const {
	return MusicSourceFolder(&session());
}

bool MusicSection::inRoster(quint64 peer) const {
	return ranges::contains(_peers, peer);
}

bool MusicSection::rosterCurrent() const {
	if (!current() || folder() != _folder) return false;
	auto &data = session().data();
	const auto &filters = data.chatsFilters().list();
	const auto filter = std::ranges::find(filters, _folder, &Data::ChatFilter::id);
	if (filter == filters.end()) return false;
	const auto peers = CaptureMusicSourceRoster(data, *filter);
	return peers && *peers == _peers;
}

void MusicSection::reset(bool keepRows) {
	++_route;
	++_query;
	_readTimer.cancel();
	_connectionTimer.cancel();
	_scanTimer.cancel();
	_waitingForScanner = false;
	_savingPage = false;
	if (_schedule->owner == this) _schedule->owner.clear();
	_scope.reset();
	_resolver.reset();
	_loadingDocument = 0;
	updatePlayback();
	_bookmarkResolver.reset();
	_actionResolver.reset();
	_pending.clear();
	_savingSelection = false;
	_cursor.reset();
	_reading = false;
	_snapshotHeld = false;
	_moreAvailable = false;
	_tail = false;
	_scanIncomplete = false;
	_configuring = false;
	_waitingMainDialogs = false;
	_waitingArchiveDialogs = false;
	_loadedMainDialogs = 0;
	_loadedArchiveDialogs = 0;
	_dialogStallTicks = 0;
	_chatsToScan = 0;
	_completedPeers.clear();
	_failedPeers.clear();
	_serverSearchErrors = 0;
	_lastServerError.clear();
	_localWriteErrors = 0;
	_indexError.clear();
	_messagesInspected = 0;
	_tracksIndexedInScan = 0;
	_skippedProtected = 0;
	_skippedUnsupported = 0;
	_skippedInvalid = 0;
	_transientNotice.clear();
	if (_request) _api.request(base::take(_request)).cancel();
	if (keepRows) {
		_more->hide(anim::type::instant);
	} else {
		_rows.clear();
		_displayedSearch.clear();
		_selection.clear();
		_totalIndexedTracks = 0;
		render();
	}
}

void MusicSection::refresh(bool force) {
	if (!current()) {
		reset();
		return;
	}
	_offline = IsOfflineState(session());
	auto &data = session().data();
	auto &filters = data.chatsFilters();
	const auto selected = folder();
	const auto &list = filters.list();
	const auto filter = std::ranges::find(list, selected, &Data::ChatFilter::id);
	if (!filters.loaded() || selected <= 0
		|| Data::IsPurpleView(selected) || filter == list.end()) {
		reset();
		_folder = 0;
		_folderName.clear();
		_peers.clear();
		updateStatus();
		return;
	}
	_folderName = filter->titleText().text;
	auto roster = InspectMusicSourceRoster(data, *filter);
	if (!roster.ready) {
		reset();
		_folder = selected;
		_peers.clear();
		_waitingMainDialogs = roster.waitingMainDialogs;
		_waitingArchiveDialogs = roster.waitingArchiveDialogs;
		_loadedMainDialogs = roster.loadedMainDialogs;
		_loadedArchiveDialogs = roster.loadedArchiveDialogs;
		updateStatus();
		requestMissingDialogs();
		return;
	}
	const auto sameSource = _folder == selected && _peers == roster.peers
		&& !_waitingMainDialogs && !_waitingArchiveDialogs;
	if (sameSource && (_configuring || _savingSelection || (!force && _scope))) {
		updateStatus();
		return;
	}
	reset(sameSource);
	_folder = selected;
	_peers = roster.peers;
	_configuring = true;
	updateStatus();
	const auto route = _route;
	ConfigureMusicIndex(&session(), selected, std::move(roster.peers),
		crl::guard(this, [=](MusicIndexConfigureResult result) {
			if (!current() || _route != route || folder() != selected) return;
			_configuring = false;
			if (!result.roster) {
				_indexError = result.error.isEmpty()
					? u"could not open local index"_q
					: result.error;
				updateStatus();
				return;
			}
			_indexError.clear();
			_scope = result.roster->scope;
			read(CatalogRead::Reload);
			if (_schedule->owner && _schedule->owner != this) {
				_waitingForScanner = true;
				updateStatus();
				return;
			}
			_schedule->owner = this;
			auto queuedPeers = std::set<quint64>();
			for (const auto &cursor : result.roster->sources) {
				if (!inRoster(cursor.peer)) continue;
				if (cursor.latest) {
					_pending.push_back({
						.saved = cursor,
						.offset = cursor.headBefore.value_or(0),
						.newest = cursor.headNewest.value_or(0),
						.head = true,
					});
					queuedPeers.insert(cursor.peer);
				} else if (!cursor.complete) {
					addBackfill(cursor);
					queuedPeers.insert(cursor.peer);
				}
			}
			_chatsToScan = int(queuedPeers.size());
			const auto retry = std::ranges::find_if(_pending, [&](const Scan &scan) {
				return scan.saved.peer == _schedule->retryPeer;
			});
			if (retry != _pending.end()) {
				std::rotate(_pending.begin(), retry, std::next(retry));
			}
			scan();
		}));
}

void MusicSection::addBackfill(MusicSourceCursor cursor) {
	if (!cursor.complete) {
		_pending.push_back({ .saved = cursor, .offset = cursor.before.value_or(0) });
	}
}

void MusicSection::scan() {
	if (!current() || !_scope || _request || _savingPage
		|| _schedule->owner != this) {
		return;
	}
	if (!rosterCurrent()) {
		refresh();
		return;
	}
	while (!_pending.empty() && !inRoster(_pending.front().saved.peer)) {
		_pending.pop_front();
	}
	_offline = IsOfflineState(session());
	if (_pending.empty()) {
		_schedule->owner.clear();
		updateStatus();
		return;
	}
	if (_offline) {
		updateStatus();
		return;
	}
	const auto now = crl::now();
	if (const auto delay = _schedule->pacer.delay(now)) {
		_scanTimer.callOnce(std::min(delay, qint64(60000)));
		updateStatus();
		return;
	}
	_schedule->pacer.sent(now);
	const auto next = _pending.front();
	_pending.pop_front();
	const auto peer = session().data().peer(PeerId(uint64(next.saved.peer)));
	const auto route = _route;
	_request = _api.request(MTPmessages_Search(
		MTP_flags(0),
		peer->input(),
		MTP_string(),
		MTP_inputPeerEmpty(),
		MTPInputPeer(),
		MTPVector<MTPReaction>(),
		MTP_int(0),
		MTP_inputMessagesFilterMusic(),
		MTP_int(0),
		MTP_int(0),
		MTP_int(next.offset),
		MTP_int(0),
		MTP_int(kSearchPageSize),
		MTP_int(0),
		MTP_int(next.head ? next.saved.latest.value_or(0) : 0),
		MTP_long(0)
	)).done([=](const MTPmessages_Messages &result) {
		if (_route != route) return;
		_request = 0;
		_offline = IsOfflineState(session());
		_schedule->pacer.succeeded();
		if (rosterCurrent() && inRoster(next.saved.peer)) {
			advance(next, result);
		} else {
			refresh();
		}
	}).fail([=](const MTP::Error &error) {
		if (_route != route) return;
		_request = 0;
		_offline = IsOfflineState(session());
		if (const auto delay = _schedule->pacer.retry(
				crl::now(), error.code(), error.type())) {
			_pending.push_front(next);
			_schedule->retryPeer = next.saved.peer;
			const auto until = QDateTime::currentMSecsSinceEpoch() + delay;
			session().local().writePref<QString>(kScanRetryPref,
				QString::number(until) + u' ' + QString::number(next.saved.peer));
			scan();
			return;
		}
		_scanIncomplete = true;
		_failedPeers.insert(next.saved.peer);
		++_serverSearchErrors;
		if (!error.type().isEmpty()) {
			_lastServerError = error.type();
		}
		if (next.head) addBackfill(next.saved);
		scan();
	}).handleAllErrors().send();
	updateStatus();
}

void MusicSection::advance(Scan next, const MTPmessages_Messages &result) {
	const auto route = _route;
	auto &data = session().data();
	const auto peer = PeerId(uint64(next.saved.peer));
	auto rows = std::vector<MusicIndexEntry>();
	auto oldest = qint64(0);
	auto anyMessages = false;
	auto boundary = false;
	const auto floor = next.head ? next.saved.latest.value_or(0) : 0;
	result.match([&](const MTPDmessages_messagesNotModified &) {
		anyMessages = true;
	}, [&](const auto &page) {
		data.processUsers(page.vusers());
		data.processChats(page.vchats());
		data.processMessages(page.vmessages().v, NewMessageType::Existing);
		anyMessages = !page.vmessages().v.empty();
		for (const auto &message : page.vmessages().v) {
			const auto id = IdFromMessage(message);
			if (PeerFromMessage(message) != peer || !id) continue;
			if (id.bare <= floor) {
				boundary = true;
				continue;
			}
			if (next.offset && id.bare >= next.offset) continue;
			if (!oldest || id.bare < oldest) oldest = id.bare;
			next.newest = std::max(next.newest, qint64(id.bare));
			if (const auto item = data.message(FullMsgId(peer, id))) {
				++_messagesInspected;
				auto inspected = InspectMusicIndexEntry(item);
				if (inspected.entry) {
					rows.push_back(std::move(*inspected.entry));
				} else {
					switch (inspected.skip) {
					case MusicSkipReason::ProtectedContent:
						++_skippedProtected;
						break;
					case MusicSkipReason::InvalidMetadata:
						++_skippedInvalid;
						break;
					default:
						++_skippedUnsupported;
						break;
					}
				}
			}
		}
	});
	if (_route != route || !_scope) return;
	if (!rosterCurrent()) {
		refresh();
		return;
	}
	if (!oldest && anyMessages && !boundary) {
		_scanIncomplete = true;
		_failedPeers.insert(next.saved.peer);
		++_serverSearchErrors;
		if (_lastServerError.isEmpty()) {
			_lastServerError = u"unrecognized search response"_q;
		}
		scan();
		return;
	}
	const auto finished = boundary || !anyMessages;
	auto saved = next.saved;
	if (next.head) {
		if (finished) {
			if (next.newest) saved.latest = next.newest;
			saved.headBefore.reset();
			saved.headNewest.reset();
		} else {
			saved.headBefore = oldest;
			saved.headNewest = next.newest;
		}
	} else {
		if (oldest) saved.before = oldest;
		if (next.newest && !saved.latest) saved.latest = next.newest;
		saved.complete = finished;
	}
	const auto hasTracks = !rows.empty();
	const auto savedCount = int(rows.size());
	_savingPage = true;
	SaveMusicIndexPage(&session(), *_scope, next.saved, saved, std::move(rows),
		crl::guard(this, [=](MusicIndexSaveResult saveResult) mutable {
			if (_route != route || !current() || folder() != _folder) return;
			_savingPage = false;
			if (!saveResult.ok) {
				_scanIncomplete = true;
				_failedPeers.insert(next.saved.peer);
				++_localWriteErrors;
				if (!saveResult.error.isEmpty()) {
					_lastServerError = saveResult.error;
				}
			} else {
				if (_schedule->retryPeer == saved.peer) {
					_schedule->retryPeer = 0;
					session().local().clearPref(kScanRetryPref);
				}
				_tracksIndexedInScan += savedCount;
				if (!finished) {
					next.saved = saved;
					next.offset = oldest;
					_pending.push_back(next);
				} else if (next.head) {
					addBackfill(saved);
					if (saved.complete) {
						_completedPeers.insert(next.saved.peer);
					}
				} else {
					_completedPeers.insert(next.saved.peer);
				}
			}
			if (saveResult.ok && hasTracks && !_snapshotHeld && !_readTimer.isActive()) {
				_readTimer.callOnce(kRefreshDelay);
			}
			scan();
		}));
}

void MusicSection::read(CatalogRead mode) {
	const auto append = mode == CatalogRead::Append;
	const auto tail = mode == CatalogRead::Tail;
	const auto text = _search->getLastText().trimmed();
	if (!current() || !_scope || folder() != _folder
		|| (append && (!_moreAvailable || _reading || text != _displayedSearch))
		|| (mode == CatalogRead::Scan && _snapshotHeld)) {
		return;
	}
	if (!rosterCurrent()) {
		refresh();
		return;
	}
	const auto query = ++_query;
	if (mode != CatalogRead::Scan) _snapshotHeld = append || tail;
	_reading = true;
	updateStatus();
	ReadMusicIndexPage(&session(), *_scope, text,
		_peers, append ? _cursor : std::nullopt, tail,
		crl::guard(this, [=](MusicIndexPageResult result) {
			if (query != _query || !current() || folder() != _folder) return;
			_reading = false;
			if (!rosterCurrent()) {
				refresh();
				return;
			}
			if (mode == CatalogRead::Scan && _snapshotHeld) return;
			if (!result.ok) {
				_indexError = result.error.isEmpty()
					? u"could not read local index"_q
					: result.error;
				updateStatus();
				return;
			}
			_indexError.clear();
			_totalIndexedTracks = result.totalTracks;
			if (!append) {
				_rows.clear();
				_cursor.reset();
			}
			_displayedSearch = text;
			_tail = tail;
			_moreAvailable = !tail && result.rows.size() == kSearchPageSize;
			if (!result.rows.empty()) {
				const auto &last = result.rows.back();
				_cursor = MusicIndexCursor{ last.postedAt, last.document };
				for (auto &entry : result.rows) {
					if (std::ranges::find(_rows, entry.document, &MusicIndexEntry::document)
						== _rows.end()) {
						_rows.push_back(std::move(entry));
					}
				}
			}
			render();
			if (tail) _list->navigate(TrackListAction::Last);
		}));
}

void MusicSection::render() {
	const auto restore = _displayedSearch.isEmpty()
		&& _search->getLastText().trimmed().isEmpty() && base::take(_restoreUnfiltered);
	const auto selected = restore ? _unfilteredCursor : _list->selectedDocument();
	const auto top = restore ? _unfilteredTop : _list->scrollTop();
	_more->toggle(current() && _moreAvailable, anim::type::instant);
	auto rows = std::vector<PlaylistSongListRow>();
	for (const auto &track : _rows) {
		if (!current() || !inRoster(track.peer)) continue;
		const auto peer = session().data().peerLoaded(PeerId(uint64(track.peer)));
		rows.push_back({
			.documentId = track.document,
			.title = track.title,
			.performer = track.performer,
			.duration = Ui::FormatDurationText(track.duration),
			.message = FullMsgId(PeerId(uint64(track.peer)), MsgId(track.message)),
			.audioDocument = track.document,
			.source = peer ? peer->name() : QString(),
		});
	}
	_list->setRows(std::move(rows), selected, top);
	updateSelection();
	updatePlayback();
	updateStatus();
	if (hasFocus()) _list->focusSelected();
}

void MusicSection::updateListHeight() {
	if (!_list) return;
	_list->setViewportLimit(std::max(1, height() - _list->y()
		- (_more ? _more->height() : 0) - (_playChosen ? _playChosen->height() : 0)));
}

void MusicSection::setStatus(const QString &text) {
	_status->entity()->setText(text);
	_status->toggle(!text.isEmpty(), anim::type::instant);
}

void MusicSection::updateStatus() {
	if (!current()) {
		setStatus(QString());
		_connectionTimer.cancel();
		return;
	}
	const auto &filters = session().data().chatsFilters();
	const auto selected = folder();
	const auto &list = filters.list();
	const auto filter = std::ranges::find(list, selected, &Data::ChatFilter::id);
	auto snapshot = MusicDiagnosticsSnapshot{
		.filtersLoaded = filters.loaded(),
		.folderSelected = selected > 0 && !Data::IsPurpleView(selected),
		.folderExists = filter != list.end(),
		.folderName = (filter != list.end()) ? filter->titleText().text : _folderName,
		.waitingMainDialogs = _waitingMainDialogs,
		.waitingArchiveDialogs = _waitingArchiveDialogs,
		.loadedMainDialogs = _loadedMainDialogs,
		.loadedArchiveDialogs = _loadedArchiveDialogs,
		.offline = _offline,
		.totalChatsInFolder = int(_peers.size()),
		.configuring = _configuring,
		.reading = _reading,
		.scanning = _request != 0 || _savingPage || !_pending.empty()
			|| _waitingForScanner,
		.chatsToScan = _chatsToScan,
		.chatsScanned = int(_completedPeers.size()),
		.retrySeconds = _schedule->pacer.retrySeconds(crl::now()),
		.chatsFailed = int(_failedPeers.size()),
		.serverSearchErrors = _serverSearchErrors,
		.lastServerError = _lastServerError,
		.localWriteErrors = _localWriteErrors,
		.indexError = _indexError,
		.messagesInspected = _messagesInspected,
		.tracksIndexedInScan = _tracksIndexedInScan,
		.skippedProtected = _skippedProtected,
		.skippedUnsupported = _skippedUnsupported,
		.skippedInvalid = _skippedInvalid,
		.displayedTracks = int(_rows.size()),
		.totalIndexedTracks = _totalIndexedTracks,
		.searchQuery = _displayedSearch.isEmpty()
			? _search->getLastText().trimmed()
			: _displayedSearch,
		.scanIncomplete = _scanIncomplete,
		.transientNotice = _transientNotice,
	};
	setStatus(DescribeMusicState(snapshot));
	const auto shouldPollConnection = !snapshot.filtersLoaded
		|| snapshot.waitingMainDialogs
		|| snapshot.waitingArchiveDialogs
		|| snapshot.configuring
		|| snapshot.scanning
		|| (snapshot.offline && snapshot.displayedTracks == 0);
	if (shouldPollConnection) {
		if (!_connectionTimer.isActive()) {
			_connectionTimer.callEach(kConnectionPollDelay);
		}
	} else {
		_connectionTimer.cancel();
	}
}

void MusicSection::requestMissingDialogs() {
	auto &data = session().data();
	if (_waitingMainDialogs && !data.chatsListLoaded()) {
		session().api().requestDialogs(nullptr);
	}
	if (_waitingArchiveDialogs) {
		if (const auto archive = data.folderLoaded(Data::Folder::kId)
			; archive && !data.chatsListLoaded(archive)) {
			session().api().requestDialogs(archive);
		}
	}
}

void MusicSection::pollConnection() {
	if (!current()) {
		_connectionTimer.cancel();
		return;
	}
	const auto offline = IsOfflineState(session());
	const auto offlineChanged = (offline != _offline);
	_offline = offline;
	if (offlineChanged || _schedule->pacer.retrySeconds(crl::now()) > 0) {
		updateStatus();
	}
	if (_offline) {
		_dialogStallTicks = 0;
		return;
	}
	if (_waitingForScanner && !_schedule->owner) {
		refresh(true);
		return;
	}
	if (offlineChanged && _schedule->owner == this) scan();
	auto &data = session().data();
	if (!data.chatsFilters().loaded()) {
		if (++_dialogStallTicks >= 3) {
			_dialogStallTicks = 0;
			data.chatsFilters().load();
		}
		return;
	}
	if (!_waitingMainDialogs && !_waitingArchiveDialogs) {
		_dialogStallTicks = 0;
		return;
	}
	const auto archive = data.folderLoaded(Data::Folder::kId);
	const auto mainDone = !_waitingMainDialogs || data.chatsListLoaded();
	const auto archiveDone = !_waitingArchiveDialogs
		|| !archive
		|| data.chatsListLoaded(archive);
	if (mainDone && archiveDone) {
		_dialogStallTicks = 0;
		refresh();
		return;
	}
	const auto mainCount = int(data.chatsList(nullptr)->indexed()->size());
	const auto archiveCount = archive
		? int(data.chatsList(archive)->indexed()->size())
		: 0;
	if (mainCount != _loadedMainDialogs || archiveCount != _loadedArchiveDialogs) {
		_loadedMainDialogs = mainCount;
		_loadedArchiveDialogs = archiveCount;
		_dialogStallTicks = 0;
		updateStatus();
		requestMissingDialogs();
	} else if (++_dialogStallTicks >= 3) {
		_dialogStallTicks = 0;
		requestMissingDialogs();
	}
}

void MusicSection::toggleTrack(quint64 documentId) {
	if (_savingSelection || _displayedSearch != _search->getLastText().trimmed()
		|| !rosterCurrent()) {
		return;
	}
	_snapshotHeld = true;
	const auto selected = std::ranges::find(_selection, documentId, &MusicIndexEntry::document);
	if (selected != _selection.end()) {
		_selection.erase(selected);
	} else {
		const auto row = std::ranges::find(_rows, documentId, &MusicIndexEntry::document);
		if (row != _rows.end() && inRoster(row->peer)) {
			_selection.push_back(*row);
		}
	}
	updateSelection();
}

void MusicSection::markTracks(std::vector<uint64_t> ids) {
	if (_savingSelection || !rosterCurrent()
		|| _displayedSearch != _search->getLastText().trimmed()) {
		return;
	}
	_snapshotHeld = true;
	const auto keys = std::set<uint64_t>(ids.begin(), ids.end());
	for (const auto &row : _rows) {
		if (keys.contains(row.document)) _selection.push_back(row);
	}
	updateSelection();
}

void MusicSection::updateSelection() {
	auto visible = std::set<uint64_t>();
	for (const auto &row : _rows) visible.insert(row.document);
	_selectionCount = int(_selection.size());
	_hiddenSelectionCount = int(std::ranges::count_if(_selection, [&](const auto &entry) {
		return !visible.contains(entry.document);
	}));
	_playChosen->setEnabled(!_savingSelection);
	if (_list) {
		auto chosen = std::set<uint64_t>();
		for (const auto &entry : _selection) chosen.insert(entry.document);
		_list->setChosenSongs(std::move(chosen));
	}
	updateListHeight();
}

void MusicSection::updatePlayback() {
	if (!_list) return;
	const auto state = Media::Player::instance()->getState(AudioMsgId::Type::Song);
	const auto document = state.id.audio();
	auto id = quint64(0);
	auto playback = MusicRowPlayback::Idle;
	if (document && &document->session() == &session()
		&& !Media::Player::IsStoppedOrStopping(state.state)) {
		id = document->id;
		playback = Media::Player::IsPausedOrPausing(state.state)
			? MusicRowPlayback::Paused
			: (state.waitingForData || state.state == Media::Player::State::Starting)
			? MusicRowPlayback::Loading
			: MusicRowPlayback::Playing;
	}
	_list->setMusicPlayback(id, playback, _loadingDocument);
}

bool MusicSection::playSelection(bool visibleOnly) {
	if (_selection.empty()) return false;
	if (_savingSelection) return true;
	if (!rosterCurrent()) {
		refresh();
		return true;
	}
	auto tracks = std::vector<MusicIndexEntry>();
	auto marked = std::set<uint64_t>();
	for (const auto &entry : _selection) marked.insert(entry.document);
	for (const auto &entry : _rows) {
		if (inRoster(entry.peer) && marked.contains(entry.document)) {
			tracks.push_back(entry);
		}
	}
	if (tracks.empty() || (!visibleOnly && tracks.size() != _selection.size())
		|| _displayedSearch != _search->getLastText().trimmed()) {
		_transientNotice = u"Clear the filter or clear hidden marks to play the selection."_q;
		updateStatus();
		return true;
	}
	auto requests = std::vector<PlaylistResolveSong>();
	requests.reserve(tracks.size());
	for (const auto &entry : tracks) requests.push_back(resolveSong(entry));
	_savingSelection = true;
	_loadingDocument = 0;
	updatePlayback();
	updateSelection();
	_transientNotice = tr::lng_purple_playlists_resolving(tr::now);
	updateStatus();
	const auto route = _route;
	_resolver = std::make_unique<DesktopPlaylistResolver>(
		&session(), std::move(requests),
		crl::guard(this, [=](std::vector<PlaylistSongResolution> result) {
			if (_route != route) return;
			if (!rosterCurrent() || result.size() != tracks.size()
				|| !std::ranges::all_of(result, [](const auto &entry) {
					return entry.status == PlaylistSongStatus::Ready;
				})) {
				_savingSelection = false;
				updateSelection();
				_transientNotice = tr::lng_purple_music_track_unavailable(tr::now);
				updateStatus();
				return;
			}
			saveSelection(tracks, visibleOnly);
		}));
	_resolver->start();
	return true;
}

void MusicSection::saveSelection(
		const std::vector<MusicIndexEntry> &tracks,
		bool visibleOnly) {
	auto songs = std::vector<PlaylistAddSongInput>();
	songs.reserve(tracks.size());
	for (const auto &entry : tracks) {
		const auto captured = captureTrack(entry);
		if (!captured) {
			_savingSelection = false;
			updateSelection();
			_transientNotice = tr::lng_purple_music_track_unavailable(tr::now);
			updateStatus();
			return;
		}
		songs.push_back(*captured);
	}
	if (tracks.size() == 1) {
		_savingSelection = false;
		if (!visibleOnly) _selection.clear();
		updateSelection();
		_transientNotice.clear();
		updateStatus();
		playResolved(tracks.front());
		return;
	}
	_transientNotice = tr::lng_purple_playlists_saving(tr::now);
	updateStatus();
	auto entropy = QByteArray(kPlaylistIdEntropyBytes, char(0));
	base::RandomFill(entropy.data(), entropy.size());
	const auto route = _route;
	EditAccountPlaylists(&session(), {
		.kind = PlaylistAccountEditKind::CreateHyper,
		.entropy = std::move(entropy),
		.songs = std::move(songs),
		.nowMs = uint64_t(QDateTime::currentMSecsSinceEpoch()),
	}, crl::guard(this, [=](PlaylistAccountResult result) {
		if (_route != route) return;
		_savingSelection = false;
		if (result && result.edit && rosterCurrent()
			&& std::ranges::all_of(tracks, [=](const auto &entry) { return bool(captureTrack(entry)); })) {
			if (!visibleOnly) _selection.clear();
			_transientNotice.clear();
			updateStatus();
			updateSelection();
			PlayAccountPlaylist(&session(), result.edit->playlistId, tracks.front().document);
			return;
		}
		_transientNotice = result
			? tr::lng_purple_music_track_unavailable(tr::now)
			: tr::lng_purple_playlists_write_failed(tr::now);
		updateStatus();
		updateSelection();
	}));
}

void MusicSection::play(quint64 documentId) {
	if (_savingSelection) return;
	if (_displayedSearch != _search->getLastText().trimmed()) return;
	if (!rosterCurrent()) {
		refresh();
		return;
	}
	const auto row = std::ranges::find(_rows, documentId, &MusicIndexEntry::document);
	if (row == _rows.end() || !inRoster(row->peer)) return;
	const auto entry = *row;
	if (captureTrack(entry)) {
		_resolver.reset();
		_loadingDocument = 0;
		playResolved(entry);
		return;
	}
	const auto route = _route;
	_loadingDocument = documentId;
	updatePlayback();
	_resolver = std::make_unique<DesktopPlaylistResolver>(
		&session(), std::vector{ resolveSong(entry) }, crl::guard(this, [=](auto results) {
			if (_route != route) return;
			_loadingDocument = 0;
			updatePlayback();
			if (!rosterCurrent()) return;
			if (results.size() == 1 && results.front().status == PlaylistSongStatus::Ready) {
				playResolved(entry);
			} else {
				_transientNotice = tr::lng_purple_music_track_unavailable(tr::now);
				updateStatus();
			}
		}));
	_resolver->start();
}

void MusicSection::copyOrgLink(quint64 documentId) {
	if (!rosterCurrent() || !isVisible()
		|| _displayedSearch != _search->getLastText().trimmed()) return;
	const auto row = std::ranges::find(_rows, documentId, &MusicIndexEntry::document);
	if (row == _rows.end() || !inRoster(row->peer)) return;
	_snapshotHeld = true;
	const auto entry = *row;
	const auto route = _route;
	_bookmarkResolver = std::make_unique<DesktopPlaylistResolver>(
		&session(), std::vector{ resolveSong(entry) }, crl::guard(this, [=](auto results) {
			if (_route != route || !rosterCurrent() || !isVisible()
				|| Core::App().activeWindow() != &controller()->window()
				|| QApplication::applicationState() != Qt::ApplicationActive) {
				return;
			}
			const auto source = PlaylistMessage{
				FullMsgId(PeerId(uint64(entry.peer)), MsgId(entry.message)), entry.document,
			};
			const auto item = session().data().message(source.id);
			const auto bookmark = (results.size() == 1
					&& results.front().status == PlaylistSongStatus::Ready
					&& inRoster(entry.peer) && item
					&& (CopyRestrictionsDisabled() || item->allowsForward()))
				? CaptureMusicBookmark(&session(), source)
				: std::nullopt;
			const auto weakSelf = QPointer<QWidget>(this);
			if (bookmark) {
				QGuiApplication::clipboard()->setText(MusicBookmarkOrgLink(*bookmark));
				const auto msg = tr::lng_username_copied(tr::now);
				ShowPurpleAck({
					.session = &session(),
					.scope = u"Music"_q,
					.text = msg,
					.severity = ModalNotices::NoticeSeverity::Info,
					.fallbackToast = [=] { if (weakSelf) { Ui::Toast::Show(weakSelf.data(), msg); } },
				});
			} else {
				const auto msg = tr::lng_purple_music_track_unavailable(tr::now);
				ShowPurpleAck({
					.session = &session(),
					.scope = u"Music"_q,
					.text = msg,
					.severity = ModalNotices::NoticeSeverity::Error,
					.fallbackToast = [=] { if (weakSelf) { Ui::Toast::Show(weakSelf.data(), msg); } },
				});
			}
		}));
	_bookmarkResolver->start();
}

void MusicSection::fillTrackActions(not_null<Ui::PopupMenu*> menu, uint64_t key) {
	menu->addAction(tr::lng_purple_playlists_play(tr::now), [=] { play(key); });
	menu->addAction(u"Add to queue"_q, [=] { act(MusicTrackAction::Queue, key); });
	menu->addAction(tr::lng_context_forward_msg(tr::now), [=] { act(MusicTrackAction::Forward, key); });
	menu->addAction(tr::lng_context_to_msg(tr::now), [=] { act(MusicTrackAction::GoTo, key); });
	menu->addAction(tr::lng_purple_playlists_add(tr::now),
		[=] { act(MusicTrackAction::AddToPlaylist, key); });
	menu->addAction(tr::lng_context_copy_link(tr::now),
		[=] { act(MusicTrackAction::CopyMessageLink, key); });
	if (!_selection.empty()) {
		menu->addAction(u"Clear marks"_q, [=] {
			_selection.clear();
			updateSelection();
		});
	}
	if (_hiddenSelectionCount.current()) {
		if (_hiddenSelectionCount.current() < _selectionCount.current()) {
			menu->addAction(u"Play visible marks"_q, [=] { playSelection(true); });
		}
		menu->addAction(u"Clear hidden marks"_q, [=] {
			std::erase_if(_selection, [=](const MusicIndexEntry &entry) {
				return std::ranges::find(_rows, entry.document, &MusicIndexEntry::document)
					== _rows.end();
			});
			updateSelection();
		});
	}
}

void MusicSection::act(MusicTrackAction action, uint64_t key) {
	if (!_list || !rosterCurrent()
		|| _displayedSearch != _search->getLastText().trimmed()) {
		return;
	}
	if (action == MusicTrackAction::Menu) {
		_list->showActions();
		return;
	}
	if (!key) key = _list->selectedDocument();
	if ((action == MusicTrackAction::GoTo || action == MusicTrackAction::CopyMessageLink)
		&& _selection.size() > 1
		&& std::ranges::find(_selection, key, &MusicIndexEntry::document) != _selection.end()) {
		_transientNotice = u"Choose one track for this action."_q;
		updateStatus();
		return;
	}
	const auto group = (action == MusicTrackAction::Forward
		|| action == MusicTrackAction::Queue || action == MusicTrackAction::AddToPlaylist)
		&& std::ranges::find(_selection, key, &MusicIndexEntry::document) != _selection.end();
	auto entries = std::vector<MusicIndexEntry>();
	for (const auto &row : _rows) {
		if (row.document == key || (group
			&& std::ranges::find(_selection, row.document, &MusicIndexEntry::document)
				!= _selection.end())) {
			entries.push_back(row);
		}
	}
	if (group && entries.size() != _selection.size()) {
		_transientNotice = u"Clear the filter or clear hidden marks first."_q;
		updateStatus();
		return;
	}
	if (entries.empty()) return;
	auto requests = std::vector<PlaylistResolveSong>();
	for (const auto &entry : entries) requests.push_back(resolveSong(entry));
	const auto route = _route;
	const auto query = _query;
	_actionResolver = std::make_unique<DesktopPlaylistResolver>(
		&session(), std::move(requests), crl::guard(this, [=](auto results) {
			if (route != _route || query != _query || !rosterCurrent()) return;
			auto messages = std::vector<PlaylistMessage>();
			for (const auto &entry : entries) {
				if (!captureTrack(entry)) break;
				messages.push_back({
					FullMsgId(PeerId(uint64(entry.peer)), MsgId(entry.message)), entry.document,
				});
			}
			if (messages.size() != entries.size() || results.size() != entries.size()) {
				_transientNotice = tr::lng_purple_music_track_unavailable(tr::now);
				updateStatus();
				return;
			}
			performTrackAction(action, messages);
		}));
	_actionResolver->start();
}

void MusicSection::performTrackAction(
		MusicTrackAction action,
		const std::vector<PlaylistMessage> &messages) {
	switch (action) {
	case MusicTrackAction::AddToPlaylist:
		ShowPlaylistPicker(controller(), messages);
		return;
	case MusicTrackAction::Queue:
		_transientNotice = AppendAccountMusicQueue(&session(), messages)
			? u"Added to the playback queue."_q
			: tr::lng_purple_music_track_unavailable(tr::now);
		break;
	case MusicTrackAction::Forward: {
		auto items = MessageIdsList();
		for (const auto &message : messages) {
			const auto source = session().data().message(message.id);
			if (!source || !source->allowsForward()) break;
			items.push_back(message.id);
		}
		if (items.size() == messages.size()) {
			if (Window::ShowForwardMessagesBox(controller(), std::move(items))) return;
		}
		_transientNotice = u"Some of these tracks cannot be forwarded."_q;
	} break;
	case MusicTrackAction::GoTo:
		if (const auto source = session().data().message(messages.front().id)) {
			controller()->showMessage(source, Window::SectionShow::Way::Forward);
			return;
		}
		_transientNotice = tr::lng_purple_music_track_unavailable(tr::now);
		break;
	case MusicTrackAction::CopyMessageLink:
		if (const auto source = session().data().message(messages.front().id);
			source && source->hasDirectLink() && !IsAnchoredEphemeral(source)) {
			HistoryView::CopyPostLink(controller(), source->fullId(), HistoryView::Context::History);
			return;
		}
		_transientNotice = u"No message link is available for this track."_q;
		break;
	case MusicTrackAction::Menu:
		return;
	}
	updateStatus();
}

PlaylistResolveSong MusicSection::resolveSong(const MusicIndexEntry &entry) const {
	const auto peer = session().data().peer(PeerId(uint64(entry.peer)));
	const auto token = peer->isSelf()
		? PeerToken{ PeerTokenKind::Self, 0 }
		: peerIsUser(peer->id)
		? PeerToken{ PeerTokenKind::User, peerToUser(peer->id).bare }
		: peerIsChat(peer->id)
		? PeerToken{ PeerTokenKind::Chat, peerToChat(peer->id).bare }
		: PeerToken{ PeerTokenKind::Channel, peerToChannel(peer->id).bare };
	return {
		.documentId = entry.document,
		.source = {
			.peer = token,
			.msg = int32_t(entry.message),
			.username = peer->isChannel() ? peer->username() : QString(),
			.since = kMinimumPlaylistStamp,
		},
	};
}

std::optional<PlaylistAddSongInput> MusicSection::captureTrack(
		const MusicIndexEntry &entry) const {
	const auto id = FullMsgId(PeerId(uint64(entry.peer)), MsgId(entry.message));
	return inRoster(entry.peer)
		? CapturePlaylistSong(&session(), { id, entry.document })
		: std::nullopt;
}

void MusicSection::playResolved(const MusicIndexEntry &entry) {
	const auto id = FullMsgId(PeerId(uint64(entry.peer)), MsgId(entry.message));
	const auto item = session().data().message(id);
	if (!item || !captureTrack(entry)) {
		_transientNotice = tr::lng_purple_music_track_unavailable(tr::now);
		updateStatus();
		return;
	}
	_transientNotice.clear();
	updateStatus();
	const auto player = Media::Player::instance();
	const auto state = player->getState(AudioMsgId::Type::Song);
	const auto document = item->media()->document();
	if (state.id.audio() == document && !Media::Player::IsStoppedOrStopping(state.state)) {
		if (Media::Player::IsPausedOrPausing(state.state)) player->play(AudioMsgId::Type::Song);
	} else {
		player->play(AudioMsgId(document, id));
	}
	updatePlayback();
}

MusicSection *MusicForWidget(
		not_null<Window::SessionController*> controller,
		QWidget *focus) {
	for (auto widget = focus; widget; widget = widget->parentWidget()) {
		if (const auto music = dynamic_cast<MusicSection*>(widget)) {
			return music->controller() == controller ? music : nullptr;
		}
	}
	return nullptr;
}

} // namespace

void AddMusicMenuRow(
		not_null<Ui::VerticalLayout*> menu,
		not_null<Window::SessionController*> controller) {
	const auto row = menu->add(::Settings::CreateButtonWithIcon(menu,
		tr::lng_purple_music_title(), st::mainMenuButton, { &st::menuIconFile }));
	row->setClickedCallback([weak = base::make_weak(controller.get())] {
		if (const auto controller = weak.get()) {
			ShowMusicSection(controller);
		}
	});
}

bool ShowMusicSection(not_null<Window::SessionController*> controller) {
	if (!MusicWindowCurrent(controller)) return false;
	controller->showSection(std::make_shared<MusicMemento>());
	return true;
}

bool MusicAllowsModalKeys(
		not_null<Window::SessionController*> controller,
		not_null<QWidget*> focus) {
	const auto music = MusicForWidget(controller, focus);
	return music && music->allowsModalKeys(focus);
}

bool MusicSearchContains(
		not_null<Window::SessionController*> controller,
		QWidget *focus) {
	const auto music = MusicForWidget(controller, focus);
	return music && music->searchContains(focus);
}

void FocusMusicSearch(
		not_null<Window::SessionController*> controller,
		QWidget *focus,
		bool search) {
	if (const auto music = MusicForWidget(controller, focus)) {
		music->focusSearch(search);
	}
}

void NavigateMusic(
		not_null<Window::SessionController*> controller,
		QWidget *focus,
		TrackListAction action,
		int repeat) {
	if (const auto music = MusicForWidget(controller, focus)) {
		music->navigate(action, repeat);
	}
}

void ActOnMusic(
		not_null<Window::SessionController*> controller,
		QWidget *focus,
		MusicTrackAction action) {
	if (const auto music = MusicForWidget(controller, focus)) music->act(action);
}

bool ToggleMusicSection(not_null<Window::SessionController*> controller) {
	if (!MusicWindowCurrent(controller)) return false;
	for (const auto child : controller->content()->children()) {
		if (const auto music = dynamic_cast<MusicSection*>(child);
			music && music->isVisible()) {
			controller->showBackFromStack();
			return true;
		}
	}
	return ShowMusicSection(controller);
}

void UpdateMusicSidebar(QWidget *widget, bool suppress, bool restore) {
	if (!widget) return;
	const auto hiddenByMusic = widget->property("purpleMusicHidden").toBool();
	if (suppress && !widget->isHidden()) {
		widget->setProperty("purpleMusicHidden", true);
		widget->hide();
	} else if (!suppress && hiddenByMusic && restore) {
		widget->setProperty("purpleMusicHidden", QVariant());
		widget->show();
	}
}

bool MusicHidesChats(const Window::SectionWidget *section) {
	return MusicHidesInfo(section) && MusicHideChatsCurrent();
}

bool MusicHidesInfo(const Window::SectionWidget *section) {
	return dynamic_cast<const MusicSection*>(section) != nullptr;
}

bool MusicOwnsSongTransport(const Window::SectionWidget *section) {
	if (!MusicHidesInfo(section) || Core::App().passcodeLocked()) return false;
	const auto player = Media::Player::instance();
	const auto type = AudioMsgId::Type::Song;
	if (player->getActiveType() != type) return false;
	const auto state = player->getState(type);
	const auto document = state.id.audio();
	return document && &document->session() == &section->session()
		&& !Media::Player::IsStoppedOrStopping(state.state);
}

bool MusicHideChatsCurrent() {
	return !Core::App().settings().readPref<bool>(kHideChatsPref, false);
}

rpl::producer<bool> MusicHideChatsValue() {
	return rpl::single(MusicHideChatsCurrent()) | rpl::then(MusicHideChatsChanges.events());
}

void SetMusicHideChats(bool hide) {
	if (hide == MusicHideChatsCurrent()) return;
	Core::App().settings().writePref<bool>(kHideChatsPref, !hide);
	Core::App().saveSettingsDelayed();
	MusicHideChatsChanges.fire_copy(hide);
}

void ChooseMusicSourceFolder(not_null<Window::SessionController*> controller) {
	if (!MusicWindowCurrent(controller)) return;
	auto &session = controller->session();
	auto &filters = session.data().chatsFilters();
	if (!filters.loaded()) {
		filters.load();
		controller->show(Ui::MakeInformBox(tr::lng_contacts_loading(tr::now)));
		return;
	}
	auto ids = std::vector<FilterId>();
	auto labels = std::vector<QString>();
	for (const auto &filter : filters.list()) {
		if (filter.id() <= 0 || Data::IsPurpleView(filter.id())) continue;
		ids.push_back(filter.id());
		labels.push_back(filter.titleText().text);
	}
	if (ids.empty()) {
		controller->show(Ui::MakeInformBox(tr::lng_purple_music_no_folders(tr::now)));
		return;
	}
	const auto folder = MusicSourceFolder(&session);
	const auto initial = std::ranges::find(ids, folder);
	const auto selected = initial != ids.end() ? int(initial - ids.begin()) : -1;
	const auto weak = base::make_weak(controller.get());
	controller->show(Box<Ui::GenericBox>([weak, ids = std::move(ids),
			labels = std::move(labels), selected](not_null<Ui::GenericBox*> box) {
		SingleChoiceBox(box, {
			.title = tr::lng_purple_music_source(),
			.options = labels,
			.initialSelection = selected,
			.callback = [weak, ids](int index) {
				const auto window = weak.get();
				if (!window || !MusicWindowCurrent(window)
					|| index < 0 || index >= int(ids.size())) {
					return;
				}
				auto &session = window->session();
				const auto &filters = session.data().chatsFilters().list();
				if (std::ranges::find(filters, ids[index], &Data::ChatFilter::id)
					== filters.end()) {
					return;
				}
				SetMusicSourceFolder(&session, ids[index]);
				MusicFolderChanges.fire_copy(&session);
			},
		});
	}));
}

} // namespace Purple

#include "purple/purple_music_section.h"

#include "apiwrap.h"
#include "base/random.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "data/data_chat_filters.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "media/player/media_player_instance.h"
#include "mtproto/sender.h"
#include "purple/purple_config.h"
#include "purple/purple_music_index_service.h"
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
#include "ui/boxes/single_choice_box.h"
#include "ui/layers/generic_box.h"
#include "ui/text/format_values.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/section_memento.h"
#include "window/section_widget.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_window.h"
#include "styles/style_window_main_menu.h"

#include <QtCore/QDateTime>
#include <QtGui/QPainter>
#include <QtWidgets/QApplication>
#include <deque>

namespace Purple {
namespace {

constexpr auto kMusicFolder = "purple.music.source-filter";
constexpr auto kSearchPageSize = 100;
constexpr auto kRefreshDelay = crl::time(180);

rpl::event_stream<Main::Session*> MusicFolderChanges;

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

class MusicSection final : public Window::SectionWidget {
public:
	MusicSection(QWidget *parent, not_null<Window::SessionController*> controller);

	bool showInternal(
		not_null<Window::SectionMemento*> memento,
		const Window::SectionShow &params) override;
	std::shared_ptr<Window::SectionMemento> createMemento() override;
	QRect floatPlayerAvailableRect() override;
	bool floatPlayerHandleWheelEvent(QEvent *) override;

private:
	enum class CatalogRead {
		Reload,
		Append,
		Scan,
	};

	void doSetInnerFocus() override;
	void paintEvent(QPaintEvent *event) override;
	[[nodiscard]] bool current() const;
	[[nodiscard]] FilterId folder() const;
	[[nodiscard]] std::vector<quint64> visiblePeers() const;
	[[nodiscard]] bool visible(quint64 peer) const;
	[[nodiscard]] bool rosterCurrent() const;
	void reset(bool keepRows = false);
	void refresh(bool force = false);
	void scan();
	void advance(Scan scan, const MTPmessages_Messages &result);
	void addBackfill(MusicSourceCursor cursor);
	void read(CatalogRead mode);
	void render();
	void updateListHeight();
	void toggleTrack(quint64 documentId);
	void updateSelection();
	bool playSelection();
	void saveSelection(const std::vector<MusicIndexEntry> &tracks);
	[[nodiscard]] PlaylistResolveSong resolveSong(const MusicIndexEntry &entry) const;
	[[nodiscard]] std::optional<PlaylistAddSongInput> captureTrack(
		const MusicIndexEntry &entry) const;
	void play(quint64 documentId);
	void playResolved(const MusicIndexEntry &entry);

	not_null<Ui::VerticalLayout*> _content;
	not_null<Ui::InputField*> _search;
	not_null<Ui::FlatLabel*> _status;
	Ui::SettingsButton *_more = nullptr;
	Ui::SettingsButton *_playChosen = nullptr;
	QPointer<PlaylistSongList> _list;
	MTP::Sender _api;
	base::Timer _refreshTimer;
	base::Timer _readTimer;
	mtpRequestId _request = 0;
	FilterId _folder = 0;
	std::optional<MusicIndexScope> _scope;
	std::vector<quint64> _peers;
	std::vector<quint64> _visible;
	std::deque<Scan> _pending;
	std::vector<MusicIndexEntry> _rows;
	std::vector<MusicIndexEntry> _selection;
	rpl::variable<int> _selectionCount = 0;
	QString _displayedSearch;
	std::optional<MusicIndexCursor> _cursor;
	std::unique_ptr<DesktopPlaylistResolver> _resolver;
	uint64_t _route = 0;
	uint64_t _query = 0;
	bool _moreAvailable = false;
	bool _reading = false;
	bool _snapshotHeld = false;
	bool _scanIncomplete = false;
	bool _configuring = false;
	bool _savingSelection = false;

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
, _search(_content->add(object_ptr<Ui::InputField>(
	_content,
	st::defaultInputField,
	Ui::InputField::Mode::SingleLine,
	tr::lng_purple_music_search())))
, _status(_content->add(object_ptr<Ui::FlatLabel>(_content, QString(), st::boxLabel)))
, _api(&session().mtp())
, _refreshTimer([=] { refresh(); })
, _readTimer([=] {
	read(_search->getLastText().trimmed() == _displayedSearch
		? CatalogRead::Scan : CatalogRead::Reload);
}) {
	const auto addButton = [=](rpl::producer<QString> title, Fn<void()> callback) {
		const auto button = _content->add(::Settings::CreateButtonWithIcon(
			_content, std::move(title), st::settingsButton, {}));
		button->setClickedCallback(std::move(callback));
		return button;
	};
	addButton(tr::lng_purple_music_back(), [=] { controller->showBackFromStack(); });
	addButton(tr::lng_purple_music_source(), [=] { ChooseMusicSourceFolder(controller); });
	addButton(tr::lng_purple_music_refresh(), [=] { refresh(true); });
	addButton(tr::lng_purple_playlists_title(), [=] {
		if (current()) ShowPlaylistsBox(controller);
	});
	_more = addButton(tr::lng_purple_music_more(), [=] {
		read(CatalogRead::Append);
	});
	_more->hide();
	_playChosen = addButton(
		tr::lng_purple_music_play_selected(lt_count, _selectionCount.value()
			| rpl::map([](int count) { return float64(count); })
			| tr::to_count()),
		[=] { playSelection(); });
	_playChosen->hide();
	widthValue() | rpl::on_next([=](int width) {
		_content->resizeToWidth(width);
	}, lifetime());
	heightValue() | rpl::on_next([=] { updateListHeight(); }, lifetime());
	_status->heightValue() | rpl::on_next([=] { updateListHeight(); }, lifetime());
	_search->changes() | rpl::on_next([=] {
		++_query;
		_reading = false;
		if (_search->getLastText().trimmed() != _displayedSearch) {
			_more->hide();
			_readTimer.callOnce(kRefreshDelay);
		} else {
			_more->setVisible(_moreAvailable);
		}
	}, lifetime());
	_search->submits() | rpl::on_next([=] {
		if (playSelection()) return;
		if (_list && !_rows.empty() && !_reading) {
			const auto selected = _list->selectedDocument();
			play(selected ? selected : _rows.front().document);
		}
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
	rpl::merge(SettingsChanges(), StateChanges())
		| rpl::on_next([=] { refresh(); }, lifetime());
	MusicFolderChanges.events() | rpl::on_next([=](Main::Session *session) {
		if (session == &this->session()) refresh();
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
	return true;
}

std::shared_ptr<Window::SectionMemento> MusicSection::createMemento() {
	return std::make_shared<MusicMemento>();
}

void MusicSection::doSetInnerFocus() {
	_search->setFocusFast();
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

bool MusicSection::current() const {
	return MusicWindowCurrent(controller());
}

FilterId MusicSection::folder() const {
	return session().local().readPref<QString>(kMusicFolder).toInt();
}

bool MusicSection::visible(quint64 peer) const {
	const auto history = session().data().historyLoaded(PeerId(uint64(peer)));
	return history && !history->purpleHiddenFromView();
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

std::vector<quint64> MusicSection::visiblePeers() const {
	auto result = std::vector<quint64>();
	for (const auto peer : _peers) {
		if (visible(peer)) result.push_back(peer);
	}
	return result;
}

void MusicSection::reset(bool keepRows) {
	++_route;
	++_query;
	_readTimer.cancel();
	_scope.reset();
	_resolver.reset();
	_pending.clear();
	_savingSelection = false;
	_cursor.reset();
	_reading = false;
	_snapshotHeld = false;
	_moreAvailable = false;
	_scanIncomplete = false;
	_configuring = false;
	if (_request) _api.request(base::take(_request)).cancel();
	if (keepRows) {
		_more->hide();
	} else {
		_rows.clear();
		_displayedSearch.clear();
		_selection.clear();
		render();
	}
}

void MusicSection::refresh(bool force) {
	if (!current()) {
		reset();
		return;
	}
	auto &data = session().data();
	auto &filters = data.chatsFilters();
	const auto selected = folder();
	const auto &list = filters.list();
	const auto filter = std::ranges::find(list, selected, &Data::ChatFilter::id);
	if (!filters.loaded() || selected <= 0
		|| Data::IsPurpleView(selected) || filter == list.end()) {
		reset();
		_status->setText(filters.loaded()
			? tr::lng_purple_music_choose_source(tr::now)
			: tr::lng_contacts_loading(tr::now));
		return;
	}
	auto peers = CaptureMusicSourceRoster(data, *filter);
	if (!peers) {
		reset();
		_status->setText(tr::lng_contacts_loading(tr::now));
		session().api().requestMoreDialogsIfNeeded();
		return;
	}
	const auto sameSource = _folder == selected && _peers == *peers
		&& _visible == visiblePeers();
	if (sameSource && (_configuring || _savingSelection || (!force && _scope))) {
		return;
	}
	reset(sameSource);
	_folder = selected;
	_peers = *peers;
	_visible = visiblePeers();
	_configuring = true;
	_status->setText(tr::lng_contacts_loading(tr::now));
	const auto route = _route;
	ConfigureMusicIndex(&session(), selected, std::move(*peers),
		crl::guard(this, [=](std::optional<MusicIndexRoster> roster) {
			if (!current() || _route != route || folder() != selected) return;
			_configuring = false;
			if (!roster) {
				_status->setText(tr::lng_purple_music_index_error(tr::now));
				return;
			}
			_scope = roster->scope;
			for (const auto &cursor : roster->sources) {
				if (!visible(cursor.peer)) continue;
				if (cursor.latest) _pending.push_back({ .saved = cursor, .head = true });
				else addBackfill(cursor);
			}
			read(CatalogRead::Reload);
			scan();
		}));
}

void MusicSection::addBackfill(MusicSourceCursor cursor) {
	if (!cursor.complete) {
		_pending.push_back({ .saved = cursor, .offset = cursor.before.value_or(0) });
	}
}

void MusicSection::scan() {
	if (!current() || !_scope || _request) return;
	if (!rosterCurrent()) {
		refresh();
		return;
	}
	while (!_pending.empty() && !visible(_pending.front().saved.peer)) {
		_pending.pop_front();
	}
	if (_pending.empty()) {
		_status->setText(_scanIncomplete
			? tr::lng_purple_music_scan_incomplete(tr::now)
			: QString());
		return;
	}
	const auto next = _pending.front();
	_pending.pop_front();
	const auto peer = session().data().peer(PeerId(uint64(next.saved.peer)));
	const auto route = _route;
	_status->setText(tr::lng_purple_music_indexing(tr::now));
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
		if (rosterCurrent() && visible(next.saved.peer)) {
			advance(next, result);
		} else {
			refresh();
		}
	}).fail([=](const MTP::Error &) {
		if (_route != route) return;
		_request = 0;
		_scanIncomplete = true;
		if (next.head) addBackfill(next.saved);
		scan();
	}).handleFloodErrors().send();
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
			const auto item = data.message(FullMsgId(peer, id));
			const auto media = item ? item->media() : nullptr;
			const auto document = media ? media->document() : nullptr;
			if (!document || item->date() <= 0 || !visible(next.saved.peer)) continue;
			const auto song = CapturePlaylistSong(&session(), { item->fullId(), document->id });
			if (!song) continue;
			rows.push_back({
				.peer = next.saved.peer,
				.message = id.bare,
				.document = document->id,
				.postedAt = item->date(),
				.title = song->meta.title,
				.performer = song->meta.performer,
				.duration = song->meta.duration,
				.searchText = song->meta.title + u' ' + song->meta.performer
					+ u' ' + document->filename(),
			});
		}
	});
	if (_route != route || !_scope) return;
	if (!rosterCurrent()) {
		refresh();
		return;
	}
	if (!oldest && anyMessages && !boundary) {
		_scanIncomplete = true;
		scan();
		return;
	}
	const auto finished = boundary || !anyMessages;
	auto saved = next.saved;
	if (next.head) {
		if (finished && next.newest) saved.latest = next.newest;
	} else {
		if (oldest) saved.before = oldest;
		if (next.newest && !saved.latest) saved.latest = next.newest;
		saved.complete = finished;
	}
	const auto hasTracks = !rows.empty();
	SaveMusicIndexPage(&session(), *_scope, next.saved, saved, std::move(rows),
		crl::guard(this, [=](bool stored) mutable {
			if (_route != route || !current() || folder() != _folder) return;
			if (!stored) {
				_scanIncomplete = true;
			} else if (!finished) {
				next.saved = saved;
				next.offset = oldest;
				_pending.push_back(next);
			} else if (next.head) {
				addBackfill(saved);
			}
			if (stored && hasTracks && !_snapshotHeld && !_readTimer.isActive()) {
				_readTimer.callOnce(kRefreshDelay);
			}
			scan();
		}));
}

void MusicSection::read(CatalogRead mode) {
	const auto append = mode == CatalogRead::Append;
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
	const auto peers = visiblePeers();
	if (peers != _visible) {
		refresh();
		return;
	}
	const auto query = ++_query;
	if (mode != CatalogRead::Scan) _snapshotHeld = append;
	_reading = true;
	ReadMusicIndexPage(&session(), *_scope, text,
		peers, append ? _cursor : std::nullopt,
		crl::guard(this, [=](std::optional<std::vector<MusicIndexEntry>> result) {
			if (query != _query || !current() || folder() != _folder) return;
			_reading = false;
			if (!rosterCurrent() || peers != visiblePeers()) {
				refresh();
				return;
			}
			if (mode == CatalogRead::Scan && _snapshotHeld) return;
			if (!result) {
				_status->setText(tr::lng_purple_music_index_error(tr::now));
				return;
			}
			if (!append) {
				_rows.clear();
				_cursor.reset();
			}
			_displayedSearch = text;
			_moreAvailable = result->size() == kSearchPageSize;
			if (!result->empty()) {
				const auto &last = result->back();
				_cursor = MusicIndexCursor{ last.postedAt, last.document };
				for (auto &entry : *result) {
					if (std::ranges::find(_rows, entry.document, &MusicIndexEntry::document)
						== _rows.end()) {
						_rows.push_back(std::move(entry));
					}
				}
			}
			render();
		}));
}

void MusicSection::render() {
	const auto selected = _list ? _list->selectedDocument() : 0;
	const auto top = _list ? _list->scrollTop() : 0;
	const auto focus = QApplication::focusWidget();
	const auto hadFocus = _list && focus
		&& (focus == _list || _list->isAncestorOf(focus));
	delete _list.data();
	_more->setVisible(current() && _moreAvailable);
	updateSelection();
	if (_rows.empty() || !current()) return;
	auto rows = std::vector<PlaylistSongListRow>();
	for (const auto &track : _rows) {
		if (!visible(track.peer)) continue;
		rows.push_back({
			.documentId = track.document,
			.title = track.title,
			.performer = track.performer,
			.duration = Ui::FormatDurationText(track.duration),
		});
	}
	_list = _content->add(object_ptr<PlaylistSongList>(
		_content, std::move(rows), selected, top,
		[=](uint64_t id) { play(id); },
		Fn<void(std::vector<uint64_t>)>(), PlaylistSongList::Mode::Library));
	_list->setSelectionActions([=](uint64_t id) { toggleTrack(id); },
		[=] { return playSelection(); });
	_list->navigationEvents() | rpl::on_next([=] {
		_snapshotHeld = true;
	}, _list->lifetime());
	updateSelection();
	if (hadFocus) _list->focusSelected();
}

void MusicSection::updateListHeight() {
	if (!_list) return;
	_list->setViewportLimit(std::max(1, height() - _list->y()));
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
		if (row != _rows.end() && visible(row->peer)) {
			_selection.push_back(*row);
		}
	}
	updateSelection();
}

void MusicSection::updateSelection() {
	_selectionCount = int(_selection.size());
	_playChosen->setVisible(current() && !_selection.empty());
	_playChosen->setDisabled(_savingSelection);
	if (_list) {
		auto chosen = std::set<uint64_t>();
		for (const auto &entry : _selection) chosen.insert(entry.document);
		_list->setChosenSongs(std::move(chosen));
	}
	updateListHeight();
}

bool MusicSection::playSelection() {
	if (_selection.empty()) return false;
	if (_savingSelection) return true;
	if (!rosterCurrent()) {
		refresh();
		return true;
	}
	auto tracks = _selection;
	if (!std::ranges::all_of(tracks, [=](const auto &entry) { return visible(entry.peer); })) {
		refresh();
		return true;
	}
	auto requests = std::vector<PlaylistResolveSong>();
	requests.reserve(tracks.size());
	for (const auto &entry : tracks) requests.push_back(resolveSong(entry));
	_savingSelection = true;
	updateSelection();
	_status->setText(tr::lng_purple_playlists_resolving(tr::now));
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
				_status->setText(tr::lng_purple_music_track_unavailable(tr::now));
				return;
			}
			saveSelection(tracks);
		}));
	_resolver->start();
	return true;
}

void MusicSection::saveSelection(const std::vector<MusicIndexEntry> &tracks) {
	auto songs = std::vector<PlaylistAddSongInput>();
	songs.reserve(tracks.size());
	for (const auto &entry : tracks) {
		const auto captured = captureTrack(entry);
		if (!captured) {
			_savingSelection = false;
			updateSelection();
			_status->setText(tr::lng_purple_music_track_unavailable(tr::now));
			return;
		}
		songs.push_back(*captured);
	}
	if (tracks.size() == 1) {
		_savingSelection = false;
		_selection.clear();
		updateSelection();
		_status->setText(QString());
		playResolved(tracks.front());
		return;
	}
	_status->setText(tr::lng_purple_playlists_saving(tr::now));
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
			_selection.clear();
			_status->setText(QString());
			updateSelection();
			PlayAccountPlaylist(&session(), result.edit->playlistId, tracks.front().document);
			return;
		}
		_status->setText(result
			? tr::lng_purple_music_track_unavailable(tr::now)
			: tr::lng_purple_playlists_write_failed(tr::now));
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
	if (row == _rows.end() || !visible(row->peer)) return;
	const auto entry = *row;
	const auto route = _route;
	_resolver = std::make_unique<DesktopPlaylistResolver>(
		&session(), std::vector{ resolveSong(entry) }, crl::guard(this, [=](auto results) {
			if (_route != route || !rosterCurrent()) return;
			if (results.size() == 1 && results.front().status == PlaylistSongStatus::Ready) {
				playResolved(entry);
			} else {
				_status->setText(tr::lng_purple_music_track_unavailable(tr::now));
			}
		}));
	_resolver->start();
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
		},
	};
}

std::optional<PlaylistAddSongInput> MusicSection::captureTrack(
		const MusicIndexEntry &entry) const {
	const auto id = FullMsgId(PeerId(uint64(entry.peer)), MsgId(entry.message));
	return visible(entry.peer)
		? CapturePlaylistSong(&session(), { id, entry.document })
		: std::nullopt;
}

void MusicSection::playResolved(const MusicIndexEntry &entry) {
	const auto id = FullMsgId(PeerId(uint64(entry.peer)), MsgId(entry.message));
	const auto item = session().data().message(id);
	if (!item || !captureTrack(entry)) {
		_status->setText(tr::lng_purple_music_track_unavailable(tr::now));
		return;
	}
	Media::Player::instance()->play(AudioMsgId(item->media()->document(), id));
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
	const auto folder = session.local().readPref<QString>(kMusicFolder).toInt();
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
				session.local().writePref<QString>(kMusicFolder, QString::number(ids[index]));
				MusicFolderChanges.fire_copy(&session);
			},
		});
	}));
}

} // namespace Purple

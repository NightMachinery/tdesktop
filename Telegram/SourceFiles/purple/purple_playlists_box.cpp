#include "purple/purple_playlists_box.h"

#include "base/random.h"
#include "base/weak_ptr.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "purple/purple_playlist_player.h"
#include "purple/purple_playlists_service.h"
#include "settings/settings_common.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/text/format_values.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QtCore/QDateTime>
#include <QtCore/QPointer>

#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_window.h"
#include "styles/style_window_main_menu.h"

namespace Purple {
namespace {

constexpr auto kPageSize = size_t(25);

class PlaylistView final : public base::has_weak_ptr {
public:
	PlaylistView(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		std::optional<PlaylistMessage> message);
	void start();

private:
	[[nodiscard]] bool current() const;
	void load(bool recordUse = false);
	void receive(PlaylistAccountResult result);
	void render();
	void showLibrary(bool deleted);
	void showDetail(const QString &id);
	void name(bool rename);
	void edit(PlaylistAccountEdit request);
	void add(const QString &id);
	void play(std::optional<uint64_t> documentId = std::nullopt);
	void playbackChanged(PlaylistPlaybackStatus status);
	void label(rpl::producer<QString> text);
	Ui::SettingsButton *button(rpl::producer<QString> text, Fn<void()> callback);
	void pages(size_t count);

	not_null<Ui::GenericBox*> _box;
	base::weak_ptr<Window::SessionController> _window;
	base::weak_ptr<Main::Session> _session;
	AccountKey _account;
	std::optional<AccountRoutingSnapshot> _route;
	std::optional<PlaylistMessage> _message;
	std::optional<PlaylistStoreRead> _snapshot;
	std::optional<PlaylistUndoRecord> _undo;
	PlaylistPlaybackStatus _playback;
	QPointer<Ui::SettingsButton> _retry;
	QString _playlistId;
	not_null<Ui::FlatLabel*> _status;
	not_null<Ui::FlatLabel*> _playbackLabel;
	not_null<Ui::VerticalLayout*> _rows;
	uint64_t _generation = 0;
	size_t _page = 0;
	bool _deleted = false;
	bool _busy = false;
	bool _closed = false;

};

[[nodiscard]] uint64_t Now() {
	return uint64_t(QDateTime::currentMSecsSinceEpoch());
}

[[nodiscard]] QByteArray Entropy() {
	auto bytes = QByteArray(kPlaylistIdEntropyBytes, char(0));
	base::RandomFill(bytes.data(), bytes.size());
	return bytes;
}

[[nodiscard]] QString StoreError(PlaylistStoreStatus status) {
	switch (status) {
	case PlaylistStoreStatus::NewerLibrary:
	case PlaylistStoreStatus::NewerState:
		return tr::lng_purple_playlists_newer(tr::now);
	case PlaylistStoreStatus::InvalidLibrary:
	case PlaylistStoreStatus::InvalidState:
		return tr::lng_purple_playlists_invalid(tr::now);
	case PlaylistStoreStatus::Busy:
		return tr::lng_purple_playlists_busy(tr::now);
	case PlaylistStoreStatus::Changed:
		return tr::lng_purple_playlists_changed(tr::now);
	default:
		return tr::lng_purple_playlists_write_failed(tr::now);
	}
}

[[nodiscard]] QString EditError(PlaylistEditError error) {
	switch (error) {
	case PlaylistEditError::InvalidTitle:
		return tr::lng_purple_playlists_bad_name(tr::now);
	case PlaylistEditError::LibraryFull:
		return tr::lng_purple_playlists_full(tr::now);
	case PlaylistEditError::UndoConflict:
		return tr::lng_purple_playlists_undo_conflict(tr::now);
	default:
		return tr::lng_purple_playlists_edit_failed(tr::now);
	}
}

[[nodiscard]] QString PlaybackText(const PlaylistPlaybackStatus &status) {
	if (status.problem != PlaylistPlaybackProblem::None) {
		switch (status.problem) {
		case PlaylistPlaybackProblem::UnsupportedMode:
			return tr::lng_purple_playlists_mode_unsupported(tr::now);
		case PlaylistPlaybackProblem::LibraryUnavailable:
			return tr::lng_purple_playlists_invalid(tr::now);
		case PlaylistPlaybackProblem::PlaylistUnavailable:
		case PlaylistPlaybackProblem::SelectedSongMissing:
			return tr::lng_purple_playlists_song_missing(tr::now);
		case PlaylistPlaybackProblem::SourceUnavailable:
			if (status.sourceStatus == PlaylistSongStatus::Deleted) {
				return tr::lng_purple_playlists_source_deleted(tr::now);
			} else if (status.sourceStatus == PlaylistSongStatus::NoAccess) {
				return tr::lng_purple_playlists_source_no_access(tr::now);
			} else if (status.sourceStatus == PlaylistSongStatus::Changed) {
				return tr::lng_purple_playlists_source_changed(tr::now);
			} else if (status.sourceStatus == PlaylistSongStatus::Restricted) {
				return tr::lng_purple_playlists_source_restricted(tr::now);
			}
			return tr::lng_purple_playlists_source_failed(tr::now);
		case PlaylistPlaybackProblem::TransientFailure:
			return tr::lng_purple_playlists_retry_required(tr::now);
		case PlaylistPlaybackProblem::PlaybackRejected:
			return tr::lng_purple_playlists_play_rejected(tr::now);
		case PlaylistPlaybackProblem::None: break;
		}
	}
	switch (status.phase) {
	case PlaylistPlaybackPhase::Loading:
	case PlaylistPlaybackPhase::Resolving:
		return tr::lng_purple_playlists_resolving(tr::now);
	case PlaylistPlaybackPhase::Playing:
		return status.sourceStatus == PlaylistSongStatus::EarlierCopy
			? tr::lng_purple_playlists_earlier_copy(tr::now)
			: tr::lng_purple_playlists_playing(tr::now);
	case PlaylistPlaybackPhase::RetryRequired:
		return tr::lng_purple_playlists_retry_required(tr::now);
	case PlaylistPlaybackPhase::Exhausted:
		return tr::lng_purple_playlists_exhausted(tr::now);
	case PlaylistPlaybackPhase::Retired:
		return tr::lng_purple_playlists_changed(tr::now);
	case PlaylistPlaybackPhase::Inactive:
		return QString();
	}
	return QString();
}

PlaylistView::PlaylistView(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		std::optional<PlaylistMessage> message)
: _box(box)
, _window(base::make_weak(controller.get()))
, _session(base::make_weak(&controller->session()))
, _account(SessionAccountKey(&controller->session()))
, _route(CaptureAccountSyncRouting(&controller->session()))
, _message(std::move(message))
, _status(box->addRow(object_ptr<Ui::FlatLabel>(box, QString(), st::boxLabel)))
, _playbackLabel(box->addRow(object_ptr<Ui::FlatLabel>(box, QString(), st::boxLabel)))
, _rows(box->addRow(object_ptr<Ui::VerticalLayout>(box), style::margins())) {
}

bool PlaylistView::current() const {
	const auto session = _session.get();
	const auto controller = _window.get();
	return !_closed && session && controller && _route
		&& &controller->session() == session
		&& controller->window().sessionController() == controller
		&& session->account().maybeSession() == session
		&& !session->account().loggingOut()
		&& SessionAccountKey(session) == _account
		&& AccountSyncRoutingCurrent(session, *_route);
}

void PlaylistView::start() {
	_box->setTitle(_message
		? tr::lng_purple_playlists_add() : tr::lng_purple_playlists_title());
	_box->setWidth(st::boxWideWidth);
	const auto weak = base::make_weak(this);
	_box->boxClosing() | rpl::on_next([=] {
		if (const auto self = weak.get()) {
			self->_closed = true;
			++self->_generation;
		}
	}, _box->lifetime());
	_box->addButton(tr::lng_close(), [box = _box] { box->closeBox(); });
	_box->addButton(tr::lng_purple_playlists_refresh(), [=] {
		if (const auto self = weak.get()) self->load();
	});
	if (!current()) {
		_status->setText(tr::lng_purple_playlists_changed(tr::now));
		return;
	}
	const auto guard = QPointer<Ui::GenericBox>(_box.get());
	_session.get()->lifetime().add([guard] {
		if (guard) guard->closeBox();
	});
	_window.get()->window().sessionControllerValue() | rpl::on_next([=] {
		const auto self = weak.get();
		if (self && !self->current()) {
			self->_box->closeBox();
		}
	}, _box->lifetime());
	SettingsChanges() | rpl::on_next([=] {
		const auto self = weak.get();
		if (self && !self->current()) self->_box->closeBox();
	}, _box->lifetime());
	AccountPlaylistPlaybackValue(_session.get()) | rpl::on_next([=](auto status) {
		const auto self = weak.get();
		if (self && self->current()) {
			self->playbackChanged(std::move(status));
		}
	}, _box->lifetime());
	load();
}

void PlaylistView::load(bool recordUse) {
	if (!current() || _busy) {
		return;
	}
	_busy = true;
	_rows->setEnabled(false);
	_status->setText(tr::lng_purple_playlists_loading(tr::now));
	const auto generation = ++_generation;
	const auto weak = base::make_weak(this);
	auto done = [=](PlaylistAccountResult result) {
		const auto self = weak.get();
		if (self && self->current() && generation == self->_generation) {
			self->receive(std::move(result));
		}
	};
	if (recordUse) {
		RecordAccountPlaylistUse(_session.get(), { _playlistId, Now() }, std::move(done));
	} else {
		ReadAccountPlaylists(_session.get(), std::move(done));
	}
}

void PlaylistView::receive(PlaylistAccountResult result) {
	const auto ok = bool(result);
	const auto snapshotStatus = result.snapshot.status;
	_busy = false;
	if (result.snapshot) {
		_snapshot = std::move(result.snapshot);
	} else {
		_snapshot.reset();
	}
	if (!ok) {
		_box->scrollToY(0);
		_status->setText(result.edit && !*result.edit
			? EditError(result.edit->error)
			: StoreError(result.write && !*result.write
				? result.write->status : snapshotStatus));
	} else {
		_status->setText(tr::lng_purple_playlists_local_only(tr::now));
	}
	if (ok && result.edit && result.edit->changed) {
		_undo = result.edit->undo;
	}
	render();
}

void PlaylistView::label(rpl::producer<QString> text) {
	_rows->add(object_ptr<Ui::FlatLabel>(_rows, std::move(text), st::boxLabel), st::boxRowPadding);
}

Ui::SettingsButton *PlaylistView::button(
		rpl::producer<QString> text,
		Fn<void()> callback) {
	const auto result = ::Settings::AddButtonWithIcon(
		_rows, std::move(text), st::settingsButtonNoIcon);
	result->setClickedCallback(std::move(callback));
	return result;
}

void PlaylistView::pages(size_t count) {
	const auto weak = base::make_weak(this);
	if (_page > 0) {
		button(tr::lng_purple_playlists_previous_page(), [=] {
			if (const auto self = weak.get()) {
				--self->_page;
				self->load();
			}
		});
	}
	if ((_page + 1) * kPageSize < count) {
		button(tr::lng_purple_playlists_next_page(), [=] {
			if (const auto self = weak.get()) {
				++self->_page;
				self->load();
			}
		});
	}
}

void PlaylistView::render() {
	_retry = nullptr;
	_rows->clear();
	playbackChanged(_playback);
	_rows->setEnabled(true);
	if (!_snapshot) {
		return;
	}
	const auto weak = base::make_weak(this);
	const auto &library = *_snapshot->library;
	if (_playlistId.isEmpty()) {
		button(tr::lng_purple_playlists_new(), [=] {
			if (const auto self = weak.get()) self->name(false);
		});
		if (!_message) {
			button(_deleted ? tr::lng_purple_playlists_title() : tr::lng_purple_playlists_deleted(), [=] {
				if (const auto self = weak.get()) self->showLibrary(!self->_deleted);
			});
		}
		auto entries = ListVisiblePlaylists(library, _snapshot->state->recentUse);
		if (_deleted) {
			entries.clear();
			for (const auto &[id, playlist] : library.playlists) {
				if (!playlist.isPartial() && playlist.deleted->value) {
					entries.push_back({ id, playlist.title->value });
				}
			}
		}
		_page = std::min(_page, entries.empty() ? size_t(0) : (entries.size() - 1) / kPageSize);
		const auto stop = std::min(entries.size(), (_page + 1) * kPageSize);
		if (entries.empty()) label(tr::lng_purple_playlists_empty());
		for (auto i = _page * kPageSize; i < stop; ++i) {
			const auto entry = entries[i];
			auto text = entry.title;
			if (!_deleted) text += u" ("_q + QString::number(qulonglong(entry.visibleSongCount)) + u")"_q;
			if (_message) {
				const auto &playlist = library.playlists.at(entry.id);
				const auto song = playlist.songs->find(_message->documentId);
				if (song != end(*playlist.songs) && song->second.value.source) {
					text = tr::lng_purple_playlists_contains(tr::now, lt_name, text);
				}
			}
			button(rpl::single(text), [=] {
				const auto self = weak.get();
				if (!self) return;
				if (self->_message) self->add(entry.id);
				else if (self->_deleted) self->edit({ .kind = PlaylistAccountEditKind::Restore, .playlistId = entry.id });
				else self->showDetail(entry.id);
			});
		}
		pages(entries.size());
	} else {
		button(tr::lng_purple_playlists_back(), [=] {
			if (const auto self = weak.get()) self->showLibrary(false);
		});
		const auto i = library.playlists.find(_playlistId);
		if (i == end(library.playlists) || i->second.isPartial() || i->second.deleted->value) {
			label(tr::lng_purple_playlists_not_found());
			return;
		}
		const auto &playlist = i->second;
		label(rpl::single(playlist.title->value));
		const auto songs = PlaylistVisibleSongIds(playlist);
		button(tr::lng_purple_playlists_play(), [=] {
			if (const auto self = weak.get()) self->play();
		})->setDisabled(songs.empty());
		_retry = button(tr::lng_purple_playlists_retry(), [=] {
			const auto self = weak.get();
			if (self && self->current()
				&& self->_playback.playlistId == self->_playlistId
				&& self->_playback.phase == PlaylistPlaybackPhase::RetryRequired) {
				RetryAccountPlaylistPlayback(self->_session.get());
			}
		});
		playbackChanged(_playback);
		button(tr::lng_purple_playlists_rename(), [=] {
			if (const auto self = weak.get()) self->name(true);
		});
		button(tr::lng_purple_playlists_copy(), [=] {
			if (const auto self = weak.get()) {
				self->edit({ .kind = PlaylistAccountEditKind::Copy, .entropy = Entropy(), .source = playlist });
			}
		});
		button(tr::lng_purple_playlists_delete(), [=] {
			if (const auto self = weak.get()) {
				self->edit({ .kind = PlaylistAccountEditKind::Delete, .playlistId = self->_playlistId });
			}
		});
		_page = std::min(_page, songs.empty() ? size_t(0) : (songs.size() - 1) / kPageSize);
		const auto stop = std::min(songs.size(), (_page + 1) * kPageSize);
		if (songs.empty()) label(tr::lng_purple_playlists_empty_songs());
		for (auto n = _page * kPageSize; n < stop; ++n) {
			const auto id = songs[n];
			const auto &meta = playlist.songs->at(id).value.source->meta;
			const auto title = meta.title.isEmpty() ? tr::lng_purple_playlists_untitled(tr::now) : meta.title;
			button(rpl::single(title), [=] {
				if (const auto self = weak.get()) self->play(id);
			});
			label(rpl::single(meta.performer + u" · "_q + Ui::FormatDurationText(meta.duration)));
			button(tr::lng_purple_playlists_remove(), [=] {
				if (const auto self = weak.get()) {
					self->edit({
						.kind = PlaylistAccountEditKind::RemoveSongs,
						.playlistId = self->_playlistId,
						.documentIds = { id },
					});
				}
			});
		}
		pages(songs.size());
	}
	if (_undo && _undo->hasUndo()) {
		button(tr::lng_purple_playlists_undo(), [=] {
			if (const auto self = weak.get()) {
				self->edit({ .kind = PlaylistAccountEditKind::Undo, .undo = *self->_undo });
			}
		});
	}
}

void PlaylistView::showLibrary(bool deleted) {
	if (!current() || _busy) return;
	_playlistId.clear();
	_deleted = deleted;
	_page = 0;
	_playbackLabel->setText(QString());
	load();
}

void PlaylistView::showDetail(const QString &id) {
	if (!current() || _busy) return;
	_playlistId = id;
	_page = 0;
	playbackChanged(_playback);
	load(true);
}

void PlaylistView::name(bool rename) {
	if (!current() || _busy || !_snapshot) return;
	const auto weak = base::make_weak(this);
	const auto id = _playlistId;
	const auto title = rename ? _snapshot->library->playlists.at(id).title->value : QString();
	_window.get()->show(Box<Ui::GenericBox>([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rename ? tr::lng_purple_playlists_rename() : tr::lng_purple_playlists_new());
		const auto field = box->addRow(object_ptr<Ui::InputField>(box,
			st::defaultInputField, Ui::InputField::Mode::SingleLine, tr::lng_purple_playlists_name()));
		field->setText(title);
		box->setFocusCallback([=] { field->setFocusFast(); });
		const auto submit = [=] {
			const auto self = weak.get();
			if (!self || !self->current() || self->_busy) {
				box->closeBox();
				return;
			}
			const auto name = field->getLastText().trimmed();
			const auto valid = CreatePlaylist(
				{},
				name,
				QByteArray(kPlaylistIdEntropyBytes, char(0)),
				Now(),
				self->_account.userId);
			if (!valid) {
				field->showError();
				return;
			}
			self->edit({ .kind = rename ? PlaylistAccountEditKind::Rename : PlaylistAccountEditKind::Create,
				.playlistId = id, .title = name, .entropy = rename ? QByteArray() : Entropy() });
			box->closeBox();
		};
		field->submits() | rpl::on_next(submit, field->lifetime());
		box->addButton(tr::lng_settings_save(), submit);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

void PlaylistView::edit(PlaylistAccountEdit request) {
	if (!current() || _busy) return;
	request.nowMs = Now();
	_busy = true;
	_rows->setEnabled(false);
	const auto kind = request.kind;
	const auto generation = ++_generation;
	_status->setText(tr::lng_purple_playlists_saving(tr::now));
	const auto weak = base::make_weak(this);
	EditAccountPlaylists(_session.get(), std::move(request), [=](auto result) {
		const auto self = weak.get();
		if (!self || !self->current() || generation != self->_generation) return;
		const auto ok = bool(result);
		const auto id = result.edit ? result.edit->playlistId : QString();
		const auto added = result.edit ? result.edit->addedCount : size_t(0);
		const auto relinked = result.edit ? result.edit->relinkedCount : size_t(0);
		if (ok && (kind == PlaylistAccountEditKind::Delete || kind == PlaylistAccountEditKind::Restore)) {
			self->_playlistId.clear();
			self->_page = 0;
		}
		if (ok && kind == PlaylistAccountEditKind::Create && !self->_message) self->_playlistId = id;
		self->receive(std::move(result));
		const auto live = weak.get();
		if (!live || !live->current() || generation != live->_generation) return;
		if (ok && kind == PlaylistAccountEditKind::Undo) {
			live->_undo.reset();
			live->render();
		}
		const auto afterRender = weak.get();
		if (!afterRender || !afterRender->current()
			|| generation != afterRender->_generation) return;
		if (ok && kind == PlaylistAccountEditKind::Create && afterRender->_message) afterRender->add(id);
		if (ok && kind == PlaylistAccountEditKind::AddSongs) {
			afterRender->_box->uiShow()->showToast(added
				? tr::lng_purple_playlists_added(tr::now)
				: relinked ? tr::lng_purple_playlists_relinked(tr::now)
				: tr::lng_purple_playlists_already(tr::now));
		}
	});
}

void PlaylistView::add(const QString &id) {
	if (!current() || _busy || !_message) return;
	const auto input = CapturePlaylistSong(_session.get(), *_message);
	if (!input) {
		_status->setText(tr::lng_purple_playlists_song_missing(tr::now));
		return;
	}
	edit({ .kind = PlaylistAccountEditKind::AddSongs, .playlistId = id, .songs = { *input } });
}

void PlaylistView::play(std::optional<uint64_t> documentId) {
	if (!current() || _busy || _playlistId.isEmpty()) return;
	const auto weak = base::make_weak(this);
	const auto id = _playlistId;
	PlayAccountPlaylist(_session.get(), id, documentId);
	if (const auto self = weak.get()) {
		if (self->current() && self->_playlistId == id) self->load(true);
	}
}

void PlaylistView::playbackChanged(PlaylistPlaybackStatus status) {
	_playback = std::move(status);
	if (_retry) {
		_retry->setDisabled(_playback.playlistId != _playlistId
			|| _playback.phase != PlaylistPlaybackPhase::RetryRequired);
	}
	_playbackLabel->setText(_playback.playlistId == _playlistId
		? PlaybackText(_playback) : QString());
}

void Open(
		not_null<Window::SessionController*> controller,
		std::optional<PlaylistMessage> message) {
	controller->show(Box<Ui::GenericBox>([=](not_null<Ui::GenericBox*> box) {
		const auto view = box->lifetime().make_state<PlaylistView>(box, controller, message);
		view->start();
	}));
}

} // namespace

void AddPlaylistsMenuRow(
		not_null<Ui::VerticalLayout*> menu,
		not_null<Window::SessionController*> controller) {
	const auto row = menu->add(object_ptr<Ui::SlideWrap<Ui::SettingsButton>>(
		menu, ::Settings::CreateButtonWithIcon(menu, tr::lng_purple_playlists_title(),
			st::mainMenuButton, { &st::menuIconFile })));
	row->toggleOn(rpl::single(rpl::empty) | rpl::then(SettingsChanges()) | rpl::map([] {
		return ActiveSettings().playlists.menuItem;
	}) | rpl::distinct_until_changed());
	row->finishAnimating();
	const auto weak = base::make_weak(controller.get());
	const auto session = base::make_weak(&controller->session());
	row->entity()->setClickedCallback([weak, session] {
		const auto controller = weak.get();
		if (controller && session && &controller->session() == session.get()
			&& controller->window().sessionController() == controller) {
			ShowPlaylistsBox(controller);
		}
	});
}

void ShowPlaylistsBox(not_null<Window::SessionController*> controller) {
	Open(controller, std::nullopt);
}

void ShowPlaylistPicker(
		not_null<Window::SessionController*> controller,
		PlaylistMessage message) {
	if (CapturePlaylistSong(&controller->session(), message)) Open(controller, message);
}

} // namespace Purple

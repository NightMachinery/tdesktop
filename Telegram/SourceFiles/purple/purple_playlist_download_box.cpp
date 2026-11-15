#include "purple/purple_playlist_download_box.h"

#include "base/weak_ptr.h"
#include "crl/crl_on_main.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "purple/purple_playlist_download_desktop.h"
#include "purple/purple_playlists_service.h"
#include "settings/settings_common.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QtCore/QPointer>

#include <utility>

#include "styles/style_layers.h"
#include "styles/style_settings.h"

namespace Purple {
namespace {

class DownloadView final : public base::has_weak_ptr {
public:
	DownloadView(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		QString playlistId,
		Fn<void()> closed);
	void start();

private:
	[[nodiscard]] bool current() const;
	void request(PlaylistAccountDownloadRequest request = {});
	void choosePreference();
	void confirmRemoval();
	void startJob(bool retry);
	void retireJob();
	void progress(uint64_t generation, PlaylistDownloadProgress value);
	void updateButtons();

	not_null<Ui::GenericBox*> _box;
	base::weak_ptr<Window::SessionController> _window;
	base::weak_ptr<Main::Session> _session;
	AccountKey _account;
	std::optional<AccountRoutingSnapshot> _route;
	QString _playlistId;
	Fn<void()> _closedCallback;
	std::shared_ptr<DesktopPlaylistDownloadJob> _job;
	rpl::variable<QString> _preferenceText;
	not_null<Ui::FlatLabel*> _status;
	not_null<Ui::FlatLabel*> _progress;
	Ui::SettingsButton *_preferenceButton = nullptr;
	Ui::SettingsButton *_startButton = nullptr;
	Ui::SettingsButton *_retryButton = nullptr;
	Ui::SettingsButton *_stopButton = nullptr;
	Ui::SettingsButton *_removeButton = nullptr;
	PlaylistKeepDownloaded _preference = PlaylistKeepDownloaded::Inherit;
	uint64_t _generation = 0;
	uint64_t _jobGeneration = 0;
	bool _loading = false;
	bool _ready = false;
	bool _hasSongs = false;
	bool _running = false;
	bool _canRetry = false;
	bool _closed = false;

};

QString PreferenceText(PlaylistKeepDownloaded value) {
	switch (value) {
	case PlaylistKeepDownloaded::On:
		return tr::lng_purple_playlist_keep_on(tr::now);
	case PlaylistKeepDownloaded::Off:
		return tr::lng_purple_playlist_keep_off(tr::now);
	case PlaylistKeepDownloaded::Inherit:
		return tr::lng_purple_playlist_download_default(tr::now,
			lt_status, ActiveSettings().playlists.keepDownloaded
				? tr::lng_purple_playlist_keep_on(tr::now)
				: tr::lng_purple_playlist_keep_off(tr::now));
	}
	Unexpected("Unknown playlist download preference.");
}

DownloadView::DownloadView(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		QString playlistId,
		Fn<void()> closed)
: _box(box)
, _window(base::make_weak(controller.get()))
, _session(base::make_weak(&controller->session()))
, _account(SessionAccountKey(&controller->session()))
, _route(CaptureAccountSyncRouting(&controller->session()))
, _playlistId(std::move(playlistId))
, _closedCallback(std::move(closed))
, _status(box->addRow(object_ptr<Ui::FlatLabel>(box, QString(), st::boxLabel)))
, _progress(box->addRow(object_ptr<Ui::FlatLabel>(box, QString(), st::boxLabel))) {
}

bool DownloadView::current() const {
	const auto window = _window.get();
	const auto session = _session.get();
	return !_closed && window && session && _route
		&& &window->session() == session
		&& window->window().sessionController() == window
		&& session->account().maybeSession() == session
		&& !session->account().loggingOut()
		&& SessionAccountKey(session) == _account
		&& AccountSyncRoutingCurrent(session, *_route);
}

void DownloadView::start() {
	_box->setTitle(tr::lng_purple_playlist_downloads());
	_box->setWidth(st::boxWideWidth);
	_box->addButton(tr::lng_close(), [box = _box] { box->closeBox(); });
	const auto rows = _box->addRow(object_ptr<Ui::VerticalLayout>(_box));
	const auto weak = base::make_weak(this);
	_box->addButton(tr::lng_purple_playlists_refresh(), [weak] {
		if (const auto self = weak.get()) self->request();
	});
	auto add = [&](rpl::producer<QString> text, Fn<void()> callback) {
		const auto button = ::Settings::AddButtonWithIcon(rows,
			std::move(text), st::settingsButtonNoIcon);
		button->setClickedCallback(std::move(callback));
		return button.get();
	};
	_preferenceButton = ::Settings::AddButtonWithLabel(rows,
		tr::lng_purple_playlist_keep_downloaded(),
		_preferenceText.value(), st::settingsButtonNoIcon).get();
	_preferenceButton->setClickedCallback([weak] {
		if (const auto self = weak.get()) self->choosePreference();
	});
	_startButton = add(tr::lng_purple_playlist_download_all(), [weak] {
		if (const auto self = weak.get()) self->startJob(false);
	});
	_retryButton = add(tr::lng_purple_playlists_retry(), [weak] {
		if (const auto self = weak.get()) self->startJob(true);
	});
	_stopButton = add(tr::lng_purple_playlist_stop_download(), [weak] {
		const auto self = weak.get();
		if (!self || !self->current()) return;
		self->retireJob();
		self->updateButtons();
		self->_progress->setText(tr::lng_purple_playlist_download_stopped(tr::now));
	});
	_removeButton = add(tr::lng_purple_playlist_remove_downloads(), [weak] {
		if (const auto self = weak.get()) self->confirmRemoval();
	});
	_box->boxClosing() | rpl::on_next([weak] {
		const auto self = weak.get();
		if (!self) return;
		self->_closed = true;
		++self->_generation;
		self->retireJob();
		const auto callback = self->_closedCallback;
		if (callback) crl::on_main(callback);
	}, _box->lifetime());
	if (!current()) {
		updateButtons();
		_status->setText(tr::lng_purple_playlists_changed(tr::now));
		return;
	}
	const auto box = QPointer<Ui::GenericBox>(_box.get());
	_session.get()->lifetime().add([box] {
		if (box) box->closeBox();
	});
	_window.get()->window().sessionControllerValue() | rpl::on_next([weak] {
		const auto self = weak.get();
		if (self && !self->current()) self->_box->closeBox();
	}, _box->lifetime());
	SettingsChanges() | rpl::on_next([weak] {
		const auto self = weak.get();
		if (!self) return;
		if (!self->current()) self->_box->closeBox();
		else self->_preferenceText = PreferenceText(self->_preference);
	}, _box->lifetime());
	request();
}

void DownloadView::request(PlaylistAccountDownloadRequest request) {
	if (!current() || _loading) return;
	if (request.kind != PlaylistDownloadRequestKind::Read) retireJob();
	request.playlistId = _playlistId;
	_loading = true;
	_ready = false;
	updateButtons();
	const auto weak = base::make_weak(this);
	_status->setText(tr::lng_purple_playlists_loading(tr::now));
	if (!weak) return;
	const auto generation = ++_generation;
	RequestAccountPlaylistDownloads(_session.get(), std::move(request),
		[weak, generation](PlaylistAccountResult result) {
			const auto self = weak.get();
			if (!self || !self->current() || generation != self->_generation) return;
			self->_loading = false;
			const auto ok = result && result.downloads;
			if (ok) {
				const auto &library = *result.snapshot.library;
				const auto i = library.playlists.find(self->_playlistId);
				self->_ready = i != end(library.playlists)
					&& !i->second.isPartial() && !i->second.deleted->value
					&& !library.purged.contains(self->_playlistId);
				self->_hasSongs = self->_ready && !PlaylistVisibleSongIds(i->second).empty();
				const auto &preferences = result.downloads->state.preferences;
				const auto preference = preferences.find(self->_playlistId);
				self->_preference = preference == end(preferences)
					? PlaylistKeepDownloaded::Inherit : preference->second;
				self->_preferenceText = PreferenceText(self->_preference);
				if (!weak) return;
			}
			self->updateButtons();
			self->_status->setText(ok && self->_ready
				? tr::lng_purple_playlist_download_local(tr::now)
				: tr::lng_purple_playlist_download_failed(tr::now));
		});
}

void DownloadView::choosePreference() {
	if (!current() || _loading || !_ready) return;
	const auto weak = base::make_weak(this);
	const auto original = _preference;
	_window.get()->show(Box<Ui::GenericBox>([weak, original](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_purple_playlist_keep_downloaded());
		const auto guard = QPointer<Ui::GenericBox>(box.get());
		const auto group = std::make_shared<Ui::RadiobuttonGroup>(int(original));
		for (const auto value : { PlaylistKeepDownloaded::Inherit,
			PlaylistKeepDownloaded::On, PlaylistKeepDownloaded::Off }) {
			box->addRow(object_ptr<Ui::Radiobutton>(box, group,
				int(value), PreferenceText(value), st::defaultBoxCheckbox));
		}
		box->addButton(tr::lng_settings_save(), [weak, group, guard] {
			const auto self = weak.get();
			if (self && self->current()) self->request({
				.kind = PlaylistDownloadRequestKind::Preference,
				.preference = static_cast<PlaylistKeepDownloaded>(group->current()),
			});
			if (guard) guard->closeBox();
		});
		box->addButton(tr::lng_cancel(), [box] { box->closeBox(); });
	}));
}

void DownloadView::confirmRemoval() {
	if (!current() || _loading || !_ready) return;
	const auto weak = base::make_weak(this);
	_window.get()->show(Box<Ui::GenericBox>([weak](not_null<Ui::GenericBox*> box) {
		const auto guard = QPointer<Ui::GenericBox>(box.get());
		box->setTitle(tr::lng_purple_playlist_remove_downloads());
		box->addRow(object_ptr<Ui::FlatLabel>(box,
			tr::lng_purple_playlist_download_remove_confirm(), st::boxLabel));
		box->addButton(tr::lng_purple_playlist_remove_downloads(), [weak, guard] {
			const auto self = weak.get();
			if (self && self->current()) self->request({
				.kind = PlaylistDownloadRequestKind::RemoveNow,
			});
			if (guard) guard->closeBox();
		});
		box->addButton(tr::lng_cancel(), [box] { box->closeBox(); });
	}));
}

void DownloadView::retireJob() {
	++_jobGeneration;
	_running = _canRetry = false;
	if (const auto job = std::exchange(_job, nullptr)) {
		job->cancel();
		crl::on_main([job] {});
	}
}

void DownloadView::startJob(bool retry) {
	if (!current() || _loading || !_ready || !_hasSongs || _running) return;
	if (retry && (!_job || !_canRetry)) return;
	const auto weak = base::make_weak(this);
	if (!retry) {
		retireJob();
		const auto generation = _jobGeneration;
		_job = std::make_shared<DesktopPlaylistDownloadJob>(
			_session.get(), _playlistId, [weak, generation](PlaylistDownloadProgress value) {
				crl::on_main([weak, generation, value] {
					if (const auto self = weak.get()) self->progress(generation, value);
				});
			});
	}
	_running = true;
	_canRetry = false;
	updateButtons();
	_progress->setText(tr::lng_purple_playlist_download_starting(tr::now));
	if (!weak) return;
	const auto job = _job;
	if (retry) job->retry();
	else job->start();
}

void DownloadView::progress(uint64_t generation, PlaylistDownloadProgress value) {
	if (!current() || generation != _jobGeneration) return;
	_running = !value.terminal;
	_canRetry = value.terminal && !value.cancelled && (value.failed || value.busy);
	updateButtons();
	const auto text = value.cancelled
		? tr::lng_purple_playlist_download_stopped(tr::now)
		: value.terminal && value.problem != PlaylistDownloadProblem::None
		? tr::lng_purple_playlist_download_failed(tr::now)
		: tr::lng_purple_playlist_download_progress(tr::now,
			lt_amount, QString::number(qulonglong(value.downloaded)),
			lt_total, QString::number(qulonglong(value.total)))
			+ u"\n"_q + tr::lng_purple_playlist_download_unavailable(tr::now,
				lt_amount, QString::number(qulonglong(value.unavailable)))
			+ u"\n"_q + tr::lng_purple_playlist_download_retry_count(tr::now,
				lt_amount, QString::number(qulonglong(value.failed + value.busy)));
	_progress->setText(text);
}

void DownloadView::updateButtons() {
	const auto enabled = current() && !_loading && _ready;
	_preferenceButton->setDisabled(!enabled);
	_startButton->setDisabled(!enabled || !_hasSongs || _running);
	_retryButton->setDisabled(!enabled || !_canRetry);
	_stopButton->setDisabled(!_running);
	_removeButton->setDisabled(!enabled);
}

} // namespace

void ShowPlaylistDownloadsBox(
		not_null<Window::SessionController*> controller,
		QString playlistId,
		Fn<void()> closed) {
	controller->show(Box<Ui::GenericBox>([=](not_null<Ui::GenericBox*> box) {
		const auto view = box->lifetime().make_state<DownloadView>(
			box, controller, playlistId, closed);
		view->start();
	}));
}

} // namespace Purple

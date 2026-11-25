#include "purple/purple_privacy_screen.h"

#include "base/weak_ptr.h"
#include "core/application.h"
#include "core/current_geo_location.h"
#include "core/core_settings.h"
#include "core/core_settings_proxy.h"
#include "main/main_domain.h"
#include "platform/platform_current_geo_location.h"
#include "mainwindow.h"
#include "purple/purple_config.h"
#include "purple/purple_privacy_clocks.h"
#include "purple/purple_privacy_state.h"
#include "purple/purple_privacy_weather.h"
#include "settings/settings_builder.h"
#include "storage/storage_domain.h"
#include "tray.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/tooltip.h"
#include "ui/wrap/vertical_layout.h"
#include "window/main_window.h"
#include "window/notifications_manager.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QDateTime>
#include <QtCore/QLocale>
#include <QtGui/QLinearGradient>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QFont>
#include <QtGui/QKeyEvent>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtWidgets/QApplication>

#include <algorithm>
#include <vector>

namespace Purple {
namespace {

constexpr auto kPrivacyOverlayName = "purple-privacy-overlay";

rpl::event_stream<> WeatherChanges;
PrivacyState::SurfaceTracker SurfaceState;
rpl::event_stream<bool> GlobalPrivacyChanges;
std::vector<QPointer<QWidget>> HiddenTopLevelWindows;
bool RestoringTopLevels = false;

class PrivacyOverlayWidget;
PrivacyOverlayWidget *FindOverlay(const ::Window::MainWindow *window);
void SyncAppFilterInstallation();
void RestoreSecondaryTopLevelSurfaces();

void PruneHiddenTopLevels() {
	HiddenTopLevelWindows.erase(
		std::remove_if(
			HiddenTopLevelWindows.begin(),
			HiddenTopLevelWindows.end(),
			[](const QPointer<QWidget> &w) { return w.isNull(); }),
		HiddenTopLevelWindows.end());
}

[[nodiscard]] bool HasHiddenTopLevels() {
	PruneHiddenTopLevels();
	return !HiddenTopLevelWindows.empty();
}

[[nodiscard]] bool IsRecordedHidden(const QWidget *widget) {
	return std::any_of(
		HiddenTopLevelWindows.begin(),
		HiddenTopLevelWindows.end(),
		[widget](const QPointer<QWidget> &w) { return w.data() == widget; });
}

void RecordHiddenTopLevel(QWidget *widget) {
	if (!widget || IsRecordedHidden(widget)) {
		return;
	}
	HiddenTopLevelWindows.emplace_back(widget);
}

[[nodiscard]] bool ShouldKeepTopLevelHidden(const QWidget *widget) {
	if (!widget || !widget->isWindow() || RestoringTopLevels) {
		return false;
	}
	const auto isMain = (dynamic_cast<const ::Window::MainWindow*>(widget) != nullptr);
	const auto action = PrivacyState::ClassifyTopLevelSurface(
		true,
		isMain,
		widget->windowType());
	if (action == PrivacyState::TopLevelAction::Ignore) {
		return false;
	}
	if (SurfaceState.shouldSuppressNewSurfaces()) {
		return true;
	}
	return SurfaceState.shouldKeepRecordedHidden() && IsRecordedHidden(widget);
}

void SuppressTopLevelWidget(QWidget *widget, PrivacyState::TopLevelAction action) {
	if (!widget || RestoringTopLevels) {
		return;
	}
	if (action == PrivacyState::TopLevelAction::HideAndRecordRestore) {
		RecordHiddenTopLevel(widget);
	}
	widget->hide();
	const auto weak = QPointer<QWidget>(widget);
	QTimer::singleShot(0, widget, [weak] {
		if (weak && weak->isVisible() && ShouldKeepTopLevelHidden(weak.data())) {
			weak->hide();
		}
	});
}

using PrivacyWeather::GeoPoint;
using PrivacyWeather::WeatherSnapshot;

constexpr auto kWeatherSourcePref = "privacy_weather_source";
constexpr auto kWeatherPlacePref = "privacy_weather_place_cell";
constexpr auto kWeatherUnitsPref = "privacy_weather_units";
constexpr auto kCurrentLocationInterval = qint64(6 * 60 * 60 * 1000);
constexpr auto kFreshForecast = qint64(60 * 60 * 1000);
constexpr auto kExpireForecast = qint64(24 * 60 * 60 * 1000);

QByteArray WeatherPref(std::string_view name) {
	return Core::App().settings().readPref<QByteArray>(name);
}

QByteArray WeatherSource() {
	const auto source = WeatherPref(kWeatherSourcePref);
	return source.isEmpty()
		? QByteArray(WeatherPref(PrivacyWeather::kLocationPref).isEmpty() ? "off" : "place")
		: source;
}

std::optional<GeoPoint> ConfiguredWeatherCell() {
	const auto parts = WeatherPref(kWeatherPlacePref).split(',');
	if (parts.size() != 2) {
		return std::nullopt;
	}
	auto latOk = false;
	auto lonOk = false;
	const auto lat = parts[0].toDouble(&latOk);
	const auto lon = parts[1].toDouble(&lonOk);
	return (latOk && lonOk) ? PrivacyWeather::RoundedCell(lat, lon) : std::nullopt;
}

QByteArray CellKey(const GeoPoint &cell) {
	return QByteArray::number(cell.latitude, 'f', 1)
		+ ',' + QByteArray::number(cell.longitude, 'f', 1);
}

QString TemperatureLabel(int celsius) {
	const auto units = WeatherPref(kWeatherUnitsPref);
	const auto imperial = units == "f" || ((units.isEmpty() || units == "system")
		&& QLocale().measurementSystem() == QLocale::ImperialUSSystem);
	return imperial
		? QString::number(int(std::lround(celsius * 1.8 + 32.))) + u"°F"_q
		: QString::number(celsius) + u"°C"_q;
}

bool WeatherPermitted() {
	return !Core::App().settings().proxy().isEnabled();
}

bool WeatherSettingsVisible() {
	return !Core::App().passcodeLocked() && !PrivacyScreenActiveAny();
}

class WeatherStore final : public QObject {
public:
	static WeatherStore &Instance() {
		static auto *instance = new WeatherStore();
		return *instance;
	}

	[[nodiscard]] const WeatherSnapshot *snapshot(qint64 now) const {
		return WeatherPermitted() && _snapshot && now - _updated < kExpireForecast
			? &*_snapshot
			: nullptr;
	}

	[[nodiscard]] bool locationFailed() const {
		return _locationFailed;
	}

	[[nodiscard]] bool fresh(qint64 now) const {
		return snapshot(now) && !_failed && now - _updated < kFreshForecast;
	}

	[[nodiscard]] QString status(const QDateTime &now) const {
		if (!WeatherPermitted() || WeatherSource() == "off") {
			return {};
		}
		const auto ms = now.toMSecsSinceEpoch();
		if (!snapshot(ms)) {
			return (_failed || _locationFailed) ? u"Weather unavailable"_q : QString();
		}
		const auto updated = QLocale().toString(
			QDateTime::fromMSecsSinceEpoch(_updated, now.timeZone()).time(),
			QLocale::ShortFormat);
		return (_failed
			? u"Offline · last updated %1"_q.arg(updated)
			: !fresh(ms)
			? u"Forecast may be outdated · %1"_q.arg(updated)
			: u"Updated %1"_q.arg(updated)) + u" · Open-Meteo"_q;
	}

	void ensureFresh() {
		const auto source = WeatherSource();
		const auto now = QDateTime::currentMSecsSinceEpoch();
		if (!PrivacyWeather::CanFetchForecast(
				PrivacyScreenActiveAny(),
				WeatherPermitted(),
				source != "off",
				_forecast != nullptr,
				_nextAttempt,
				now)) {
			return;
		}
		if (source == "current") {
#ifdef Q_OS_MAC
			if (!_locating && now - _lastLocationAttempt >= kCurrentLocationInterval) {
				resolveLocation(false);
			}
#endif // Q_OS_MAC
			if (_currentCell && !_locating) {
				fetch(*_currentCell);
			}
		} else if (const auto cell = ConfiguredWeatherCell()) {
			fetch(*cell);
		} else if (_legacyCell) {
			fetch(*_legacyCell);
		} else {
			resolveLegacyPlace();
		}
	}

	void sourceChanged() {
		++_generation;
		_forecast = nullptr;
		cancelRequests();
		_currentCell.reset();
		_legacyCell.reset();
		_snapshot.reset();
		_updated = 0;
		_nextAttempt = 0;
		_lastLocationAttempt = 0;
		_locating = false;
		_failed = _locationFailed = false;
		WeatherChanges.fire({});
	}

	void resolveLocation(bool fromSettings) {
		if (!WeatherPermitted() || _locating) {
			return;
		}
		_locating = true;
		_lastLocationAttempt = QDateTime::currentMSecsSinceEpoch();
		const auto generation = _generation;
		const auto done = [=](Core::GeoLocation location) {
			if (generation != _generation) {
				return;
			}
			_locating = false;
			const auto cell = location.exact()
				? PrivacyWeather::RoundedCell(location.point.x(), location.point.y())
				: std::nullopt;
			if (!cell || !_currentCell || CellKey(*cell) != CellKey(*_currentCell)) {
				_snapshot.reset();
				_nextAttempt = 0;
			}
			_currentCell = cell;
			_locationFailed = !cell;
			WeatherChanges.fire({});
			ensureFresh();
		};
#ifdef Q_OS_MAC
		if (!fromSettings) {
			Platform::ResolveCurrentAuthorizedLocation(done);
			return;
		}
#endif // Q_OS_MAC
		if (fromSettings) {
			Platform::ResolveCurrentExactLocation(done);
		}
	}

	void findPlaces(const QString &query, Fn<void(std::vector<GeoPoint>)> done) {
		const auto url = PrivacyWeather::BuildGeocodeUrl(query, 5);
		if (!url || !WeatherPermitted()) {
			done({});
			return;
		}
		const auto reply = get(*url);
		connect(reply, &QNetworkReply::finished, this, [=] {
			reply->deleteLater();
			done(reply->error() == QNetworkReply::NoError && WeatherPermitted()
				? PrivacyWeather::ParsePlaces(reply->readAll())
				: std::vector<GeoPoint>());
		});
	}

private:
	WeatherStore() {
		Core::App().settings().proxy().connectionTypeChanges(
		) | rpl::on_next([=] {
			if (!WeatherPermitted()) {
				cancelRequests();
			}
			WeatherChanges.fire({});
		}, _lifetime);
	}

	void cancelRequests() {
		if (_nam) {
			for (const auto reply : _nam->findChildren<QNetworkReply*>()) {
				if (reply->isRunning()) {
					reply->abort();
				}
			}
		}
	}

	QNetworkReply *get(const QUrl &url) {
		if (!_nam) {
			_nam = new QNetworkAccessManager(this);
		}
		auto request = QNetworkRequest(url);
		request.setRawHeader("User-Agent", PrivacyWeather::kGenericUserAgent);
		request.setRawHeader("Accept-Language", PrivacyWeather::kFixedAcceptLanguage);
		request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
			QNetworkRequest::SameOriginRedirectPolicy);
		request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
			QNetworkRequest::Manual);
		request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
			QNetworkRequest::Manual);
		request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
			QNetworkRequest::AlwaysNetwork);
		request.setTransferTimeout(8000);
		return _nam->get(request);
	}

	void resolveLegacyPlace() {
		const auto url = PrivacyWeather::BuildGeocodeUrl(QString::fromUtf8(
			WeatherPref(PrivacyWeather::kLocationPref)));
		if (!url) {
			return;
		}
		const auto reply = _forecast = get(*url);
		connect(reply, &QNetworkReply::finished, this, [=] {
			reply->deleteLater();
			if (_forecast != reply) {
				return;
			}
			_forecast = nullptr;
			if (reply->error() != QNetworkReply::NoError || !WeatherPermitted()) {
				fail(reply);
				return;
			}
			_legacyCell = PrivacyWeather::ParseGeocodeResponse(reply->readAll(), {});
			if (_legacyCell) {
				ensureFresh();
			} else {
				fail(reply);
			}
		});
	}

	void fetch(const GeoPoint &cell) {
		const auto reply = _forecast = get(PrivacyWeather::BuildForecastUrl(
			cell.latitude,
			cell.longitude));
		connect(reply, &QNetworkReply::finished, this, [=] {
			reply->deleteLater();
			if (_forecast != reply) {
				return;
			}
			_forecast = nullptr;
			if (reply->error() != QNetworkReply::NoError || !WeatherPermitted()) {
				fail(reply);
				return;
			}
			auto snapshot = PrivacyWeather::ParseForecastResponse(reply->readAll(), {});
			if (!snapshot) {
				fail(reply);
				return;
			}
			_updated = QDateTime::currentMSecsSinceEpoch();
			_nextAttempt = _updated + PrivacyWeather::kRefreshIntervalMs;
			_failed = false;
			_snapshot = std::move(snapshot);
			WeatherChanges.fire({});
		});
	}

	void fail(not_null<QNetworkReply*> reply) {
		auto ok = false;
		const auto retrySeconds = reply->rawHeader("Retry-After").toLongLong(&ok);
		const auto now = QDateTime::currentMSecsSinceEpoch();
		const auto parsedDate = QDateTime::fromString(
			QString::fromLatin1(reply->rawHeader("Retry-After")),
			Qt::RFC2822Date);
		const auto retry = ok
			? std::clamp(retrySeconds, qint64(0), qint64(86400)) * 1000
			: parsedDate.isValid()
			? parsedDate.toMSecsSinceEpoch() - now
			: 0;
		_nextAttempt = now + std::max(PrivacyWeather::kFailureBackoffMs, retry);
		_failed = true;
		WeatherChanges.fire({});
	}

	QNetworkAccessManager *_nam = nullptr;
	QNetworkReply *_forecast = nullptr;
	std::optional<GeoPoint> _currentCell;
	std::optional<GeoPoint> _legacyCell;
	std::optional<WeatherSnapshot> _snapshot;
	qint64 _lastLocationAttempt = 0;
	qint64 _nextAttempt = 0;
	qint64 _updated = 0;
	uint64 _generation = 0;
	bool _failed = false;
	bool _locationFailed = false;
	bool _locating = false;
	rpl::lifetime _lifetime;
};

void ChangeWeatherSource(QByteArray source, std::optional<GeoPoint> place = {}) {
	auto &settings = Core::App().settings();
	settings.writePref<QByteArray>(kWeatherSourcePref, source);
	settings.writePref<QByteArray>(kWeatherPlacePref, place ? CellKey(*place) : QByteArray());
	settings.writePref<QByteArray>(PrivacyWeather::kLocationPref, QByteArray());
	WeatherStore::Instance().sourceChanged();
	Core::App().saveSettingsDelayed();
}

class PrivacyOverlayWidget final : public Ui::RpWidget {
public:
	explicit PrivacyOverlayWidget(not_null<::MainWindow*> window)
	: RpWidget(window->bodyWidget())
	, _window(window)
	, _backdrop(Ui::CreateChild<PrivacyBackdropWidget>(this))
	, _returnButton(Ui::CreateChild<Ui::RoundButton>(
		this,
		rpl::single(QStringLiteral("Return")),
		st::defaultActiveButton))
	, _lockButton(Ui::CreateChild<Ui::RoundButton>(
		this,
		rpl::single(QStringLiteral("Lock with passcode")),
		st::defaultLightButton)) {
		setObjectName(QString::fromLatin1(kPrivacyOverlayName));
		setFocusPolicy(Qt::StrongFocus);
		setAttribute(Qt::WA_OpaquePaintEvent);
		_backdrop->setInteractiveHint(
			QStringLiteral("Press ESC or q to return"));
		_returnButton->setClickedCallback([=] { DismissPrivacyScreen(); });
		_lockButton->setClickedCallback([=] { triggerLockWithPasscode(); });
		_lockButton->hide();
	}

	void activate(QRect body) {
		_active = true;
		_lockButton->setVisible(
			Core::App().domain().local().hasLocalPasscode()
			&& !Core::App().passcodeLocked());
		setGeometry(body);
		layoutButtons();
		show();
		raise();
		if (QApplication::activeWindow() == _window) {
			setFocus(Qt::OtherFocusReason);
		}
		_window->updateTitle();
		_window->updateControlsGeometry();
		_window->fixOrder();
	}

	void dismissLocal() {
		if (!_active) {
			return;
		}
		_active = false;
		hide();
		_window->updateTitle();
		_window->updateControlsGeometry();
		_window->fixOrder();
		if (QApplication::activeWindow() == _window) {
			_window->setInnerFocus();
		}
	}

	[[nodiscard]] bool active() const {
		return _active;
	}

	bool filterWindowEvent(QWidget *widget, QEvent *event) {
		if (!_active) {
			return false;
		}
		// Never block the passcode/setup-email lock screen or its boxes when locked.
		if (Core::App().passcodeLocked() || _window->controller().locked()) {
			return false;
		}

		const auto type = event->type();
		const auto insideOverlay = (widget == this) || isAncestorOf(widget);

		if (type == QEvent::WindowActivate) {
			raise();
			setFocus(Qt::OtherFocusReason);
			return false;
		}
		if (type == QEvent::FocusIn && !insideOverlay) {
			setFocus(Qt::OtherFocusReason);
			return true;
		}
		if ((type == QEvent::InputMethod
				|| type == QEvent::InputMethodQuery
				|| type == QEvent::DragEnter
				|| type == QEvent::DragMove
				|| type == QEvent::Drop)
			&& !insideOverlay) {
			event->ignore();
			return true;
		}
		if (type != QEvent::KeyPress
			&& type != QEvent::KeyRelease
			&& type != QEvent::ShortcutOverride
			&& type != QEvent::Shortcut) {
			return false;
		}
		if (type == QEvent::ShortcutOverride || type == QEvent::Shortcut) {
			event->accept();
			return true;
		}
		if (type == QEvent::KeyRelease) {
			return true;
		}
		const auto *ke = static_cast<QKeyEvent*>(event);
		const auto mods = ke->modifiers() & ~Qt::KeypadModifier;
		if (!ke->isAutoRepeat() && !mods) {
			if (ke->key() == Qt::Key_Escape || ke->key() == Qt::Key_Q) {
				DismissPrivacyScreen();
			} else if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
				if (_lockButton->hasFocus() && _lockButton->isVisible()) {
					triggerLockWithPasscode();
				} else {
					DismissPrivacyScreen();
				}
			}
		}
		// Swallow every keystroke while privacy screen is active.
		return true;
	}

protected:
	void resizeEvent(QResizeEvent *e) override {
		RpWidget::resizeEvent(e);
		_backdrop->setGeometry(rect());
		layoutButtons();
	}

private:
	void triggerLockWithPasscode() {
		if (Core::App().domain().local().hasLocalPasscode()) {
			// Lock first so passcode covers every window before privacy dismisses (no chat flash).
			// Hidden auxiliary surfaces stay hidden until passcode unlock.
			Core::App().lockByPasscode();
			DismissPrivacyScreen();
		}
	}

	void layoutButtons() {
		const auto rw = std::max(120, _returnButton->width());
		_returnButton->resize(rw, _returnButton->height());
		const auto y = std::max(16, height() - _returnButton->height() - 48);
		if (_lockButton->isVisible()) {
			const auto lw = std::max(168, _lockButton->width());
			_lockButton->resize(lw, _lockButton->height());
			constexpr auto kGap = 12;
			const auto total = rw + kGap + lw;
			const auto startX = (width() - total) / 2;
			_returnButton->move(startX, y);
			_lockButton->move(startX + rw + kGap, y);
		} else {
			_returnButton->move((width() - rw) / 2, y);
		}
	}

	const not_null<::MainWindow*> _window;
	PrivacyBackdropWidget *_backdrop = nullptr;
	Ui::RoundButton *_returnButton = nullptr;
	Ui::RoundButton *_lockButton = nullptr;
	bool _active = false;
};

// Single app-level event filter for privacy screen input hardening and
// live interception of auxiliary top-level surfaces shown after activation.
class PrivacyAppFilter final : public QObject {
public:
	static PrivacyAppFilter &Instance() {
		static auto *instance = new PrivacyAppFilter(qApp);
		return *instance;
	}

	void ensurePasscodeSubscription() {
		if (_subscribedPasscode) {
			return;
		}
		_subscribedPasscode = true;
		Core::App().passcodeLockValue() | rpl::on_next([](bool locked) {
			const auto shouldRestore = SurfaceState.onPasscodeLockChanged(
				locked,
				HasHiddenTopLevels());
			if (shouldRestore) {
				RestoreSecondaryTopLevelSurfaces();
			}
			SyncAppFilterInstallation();
		}, _lifetime);
	}

	void setInstalled(bool installed) {
		if (!qApp || _installed == installed) {
			return;
		}
		_installed = installed;
		if (installed) {
			qApp->installEventFilter(this);
		} else {
			qApp->removeEventFilter(this);
		}
	}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override {
		if (!watched || !watched->isWidgetType()) {
			return QObject::eventFilter(watched, event);
		}
		auto *widget = static_cast<QWidget*>(watched);
		const auto type = event->type();

		if (!RestoringTopLevels
			&& widget->isWindow()
			&& (type == QEvent::Show || type == QEvent::WindowActivate)) {
			const auto isMain = (dynamic_cast<::Window::MainWindow*>(widget) != nullptr);
			const auto action = PrivacyState::ClassifyTopLevelSurface(
				true,
				isMain,
				widget->windowType());
			if (action != PrivacyState::TopLevelAction::Ignore
				&& (SurfaceState.shouldSuppressNewSurfaces()
					|| (SurfaceState.shouldKeepRecordedHidden()
						&& IsRecordedHidden(widget)))) {
				SuppressTopLevelWidget(widget, action);
				return false;
			}
		}

		if (SurfaceState.privacyActive) {
			if (auto *mainWindow = dynamic_cast<::Window::MainWindow*>(widget->window())) {
				if (auto *overlay = FindOverlay(mainWindow); overlay && overlay->active()) {
					if (overlay->filterWindowEvent(widget, event)) {
						return true;
					}
				}
			}
		}
		return QObject::eventFilter(watched, event);
	}

private:
	explicit PrivacyAppFilter(QObject *parent) : QObject(parent) {}

	bool _installed = false;
	bool _subscribedPasscode = false;
	rpl::lifetime _lifetime;
};

void SyncAppFilterInstallation() {
	PrivacyAppFilter::Instance().setInstalled(SurfaceState.needsAppFilter());
}

PrivacyOverlayWidget *FindOverlay(const ::Window::MainWindow *window) {
	if (!window) {
		return nullptr;
	}
	const auto body = const_cast<::Window::MainWindow*>(window)->bodyWidget();
	if (!body) {
		return nullptr;
	}
	return dynamic_cast<PrivacyOverlayWidget*>(
		body->findChild<QWidget*>(
			QString::fromLatin1(kPrivacyOverlayName),
			Qt::FindDirectChildrenOnly));
}

PrivacyOverlayWidget *EnsureOverlay(not_null<::MainWindow*> window) {
	if (const auto existing = FindOverlay(window.get())) {
		return existing;
	}
	auto *overlay = new PrivacyOverlayWidget(window);
	overlay->hide();
	return overlay;
}

void HideSecondaryTopLevelSurfaces() {
	Ui::Tooltip::Hide();
	while (auto *popup = QApplication::activePopupWidget()) {
		popup->close();
		if (QApplication::activePopupWidget() == popup) {
			popup->hide();
			break;
		}
	}
	PruneHiddenTopLevels();
	for (auto *w : QApplication::topLevelWidgets()) {
		if (!w || !w->isVisible()) {
			continue;
		}
		const auto isMain = (dynamic_cast<::Window::MainWindow*>(w) != nullptr);
		const auto action = PrivacyState::ClassifyTopLevelSurface(
			true,
			isMain,
			w->windowType());
		if (action == PrivacyState::TopLevelAction::Ignore) {
			continue;
		}
		// Hide auxiliary Purple windows (call panels, group call windows, Instant View,
		// bot web apps, custom notifications) without closing or ending active calls.
		SuppressTopLevelWidget(w, action);
	}
}

void RestoreSecondaryTopLevelSurfaces() {
	auto list = std::move(HiddenTopLevelWindows);
	HiddenTopLevelWindows.clear();
	RestoringTopLevels = true;
	for (const auto &weak : list) {
		if (weak) {
			weak->show();
		}
	}
	RestoringTopLevels = false;
}

void RefreshMenusAndTitles() {
	Core::App().tray().updateIconCounters();
	Core::App().enumerateWindows([](not_null<Window::Controller*> ctrl) {
		if (const auto w = ctrl->widget()) {
			w->updateTitle();
			w->updateGlobalMenu();
		}
	});
}

} // namespace

PrivacyBackdropWidget::PrivacyBackdropWidget(not_null<QWidget*> parent)
: RpWidget(parent) {
	setFocusPolicy(Qt::NoFocus);
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setAttribute(Qt::WA_OpaquePaintEvent);

	auto *tick = new QTimer(this);
	tick->setInterval(1000);
	connect(tick, &QTimer::timeout, this, [this, last = qint64(-1)]() mutable {
		const auto minute = QDateTime::currentMSecsSinceEpoch() / 60000;
		if (isVisible() && last != minute) {
			last = minute;
			update();
			refreshWeatherIfConfigured();
		}
	});
	connect(qApp, &QApplication::applicationStateChanged, this, [this] {
		if (isVisible()) {
			update();
			refreshWeatherIfConfigured();
		}
	});
	tick->start();

	WeatherChanges.events() | rpl::on_next([this] {
		if (isVisible()) {
			update();
		}
	}, _weatherLifetime);

	refreshWeatherIfConfigured();
}

PrivacyBackdropWidget::~PrivacyBackdropWidget() = default;

void PrivacyBackdropWidget::setInteractiveHint(const QString &hint) {
	_hint = hint;
	update();
}

void PrivacyBackdropWidget::showEvent(QShowEvent *event) {
	RpWidget::showEvent(event);
	refreshWeatherIfConfigured();
}

void PrivacyBackdropWidget::refreshWeatherIfConfigured() {
	WeatherStore::Instance().ensureFresh();
}

void PrivacyBackdropWidget::paintEvent(QPaintEvent *) {
	auto p = Painter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto r = rect();
	const auto unit = st::normalFont->height;
	const auto locale = QLocale();
	const auto utc = QDateTime::currentDateTimeUtc();
	const auto zone = QTimeZone::systemTimeZone();
	const auto now = utc.toTimeZone(zone);
	const auto ms = utc.toMSecsSinceEpoch();
	const auto &weather = WeatherStore::Instance();
	const auto forecast = weather.snapshot(ms);
	const auto status = weather.status(now);
	const auto localDay = now.time().hour() >= 7 && now.time().hour() < 19;
	const auto day = weather.fresh(ms) ? forecast->isDay.value_or(localDay) : localDay;
	const auto dark = st::windowBg->c.lightness() < 128;
	const auto ink = dark ? QColor(246, 242, 255) : QColor(30, 38, 63);
	const auto muted = dark ? QColor(219, 210, 237) : QColor(59, 65, 92);
	const auto sky = dark ? QColor(23, 25, 49) : QColor(220, 236, 250);
	const auto horizon = dark ? QColor(54, 43, 71) : QColor(251, 224, 201);
	const auto panel = dark ? QColor(36, 33, 59) : QColor(249, 246, 247);

	p.fillRect(r, sky);
	auto fade = QLinearGradient(0, 0, 0, height());
	fade.setColorAt(0., sky);
	fade.setColorAt(1., horizon);
	p.fillRect(r, fade);

	const auto wide = r.width() >= 38 * unit;
	const auto shortScreen = r.height() < 26 * unit;
	const auto bodyBottom = std::max(unit, r.bottom() - 6 * unit);
	const auto horizontalInset = unit;
	const auto body = r.adjusted(horizontalInset, unit, -horizontalInset, 0);
	const auto activeWeather = forecast || !status.isEmpty();
	const auto panelRows = forecast ? (shortScreen ? 3 : wide ? 6 : 8) : 1;
	const auto panelH = activeWeather ? panelRows * unit : 0;
	const auto clocksBottom = bodyBottom - panelH - unit;
	const auto timeSize = std::clamp(
		std::min(body.width() / 5, clocksBottom / 2),
		2 * unit,
		9 * unit);
	const auto clockBlock = timeSize + 3 * unit;
	const auto top = std::max(unit, (clocksBottom - clockBlock) / 3);

	const auto text = [&](QRect target, const QString &value, int size,
			QColor color, QFont::Weight weight = QFont::Normal) {
		auto font = st::normalFont->f;
		font.setPixelSize(size);
		font.setWeight(weight);
		font.setFeature(QFont::Tag("tnum"), 1);
		p.setFont(font);
		p.setPen(color);
		p.drawText(target, Qt::AlignCenter,
			p.fontMetrics().elidedText(value, Qt::ElideRight, target.width()));
	};

	if (wide && !shortScreen) {
		const auto disc = QRect(r.width() - 8 * unit, 3 * unit, 3 * unit, 3 * unit);
		p.setPen(Qt::NoPen);
		p.setBrush(day
			? (dark ? QColor(233, 175, 123) : QColor(245, 183, 124))
			: (dark ? QColor(221, 209, 242) : QColor(246, 247, 252)));
		p.drawEllipse(disc);
		if (!day) {
			p.setBrush(sky);
			p.drawEllipse(disc.translated(unit, -unit / 2));
		}
	}

	text(QRect(body.x(), top, body.width(), 2 * unit),
		locale.toString(now.date(), QLocale::LongFormat), unit, muted);
	text(QRect(body.x(), top + 2 * unit, body.width(), timeSize + unit),
		locale.toString(now.time(), QLocale::ShortFormat), timeSize,
		ink, QFont::Light);

	const auto clockRows = PrivacyClocks::At(utc, zone, ActiveSettings().worldClocks);
	const auto clocksTop = top + clockBlock;
	const auto clockColumns = wide ? 3 : 1;
	const auto clockWidth = body.width() / clockColumns;
	for (auto i = 0; i < int(clockRows.size()); ++i) {
		const auto y = clocksTop + (i / clockColumns) * 2 * unit;
		if (y + 2 * unit > clocksBottom || (shortScreen && i > 0)) {
			break;
		}
		const auto &clock = clockRows[i];
		const auto difference = clock.dayDifference;
		const auto relative = (difference == 1)
			? u" · Tomorrow"_q
			: (difference == -1)
			? u" · Yesterday"_q
			: (difference > 0)
			? u" · +%1 days"_q.arg(difference)
			: (difference < 0)
			? u" · %1 days"_q.arg(difference)
			: QString();
		text(QRect(body.x() + (i % clockColumns) * clockWidth, y,
				clockWidth, 2 * unit),
			u"%1  %2%3"_q.arg(clock.label,
				locale.toString(clock.time.time(), QLocale::ShortFormat), relative),
			unit, muted, QFont::Medium);
	}

	if (activeWeather) {
		const auto panelTop = bodyBottom - panelH;
		p.setBrush(panel);
		p.setPen(Qt::NoPen);
		p.drawRoundedRect(QRect(body.x(), panelTop, body.width(), panelH),
			unit, unit);
		if (forecast) {
			text(QRect(body.x(), panelTop, body.width(), 2 * unit),
				u"Local weather  ·  %1  ·  %2"_q.arg(
					TemperatureLabel(forecast->temperatureC), forecast->condition),
				unit, ink, QFont::DemiBold);
		}
		if (forecast && !shortScreen) {
			const auto hourlyColumns = wide ? 6 : 3;
			const auto span = body.width() / hourlyColumns;
			auto hour = 0;
			for (const auto &entry : forecast->hours) {
				if (entry.time <= utc) {
					continue;
				}
				const auto x = body.x() + hour * span;
				const auto y = panelTop + 2 * unit;
				auto hourLabel = locale.toString(entry.time.time(), QLocale::ShortFormat);
				const auto repeated = std::any_of(forecast->hours.begin(),
					forecast->hours.end(), [&](const auto &other) {
						return other.time.date() == entry.time.date()
							&& other.time.time() == entry.time.time()
							&& other.time.offsetFromUtc() != entry.time.offsetFromUtc();
					});
				if (repeated) {
					hourLabel += u' ' + entry.time.timeZoneAbbreviation();
				}
				text(QRect(x, y, span, unit), hourLabel, unit - unit / 5, muted);
				text(QRect(x, y + unit, span, unit),
					(wide ? u"%1 · %2 · %3%"_q.arg(
						TemperatureLabel(entry.temperatureC), entry.condition).arg(entry.precipitation)
						: u"%1 · %2%"_q.arg(TemperatureLabel(entry.temperatureC)).arg(entry.precipitation)),
					unit - unit / 5, ink);
				if (++hour == hourlyColumns) {
					break;
				}
			}

			const auto today = utc.toTimeZone(forecast->zone).date();
			auto dayIndex = 0;
			for (const auto &entry : forecast->days) {
				if (entry.date <= today) {
					continue;
				}
				const auto span = wide ? body.width() / 3 : body.width();
				const auto x = body.x() + (wide ? dayIndex * span : 0);
				const auto y = panelTop + (4 + (wide ? 0 : dayIndex)) * unit;
				text(QRect(x, y, span, unit),
					u"%1  ·  %2  ·  %3 / %4"_q.arg(
						locale.dayName(entry.date.dayOfWeek(), QLocale::ShortFormat),
						entry.condition,
						TemperatureLabel(entry.lowC),
						TemperatureLabel(entry.highC)),
					unit - unit / 5, ink);
				if (++dayIndex == 3) {
					break;
				}
			}
		}
		text(QRect(body.x(), bodyBottom - unit, body.width(), unit),
			status, unit - unit / 5, muted);
	}

	if (!_hint.isEmpty()) {
		text(QRect(body.x(), r.bottom() - 2 * unit, body.width(), unit),
			_hint, unit - unit / 5, muted);
	}
}

bool PrivacyScreenActiveAny() {
	return SurfaceState.privacyActive;
}

rpl::producer<bool> PrivacyScreenChanges() {
	return GlobalPrivacyChanges.events();
}

bool PrivacyScreenActive(const ::Window::MainWindow *window) {
	if (SurfaceState.privacyActive) {
		return true;
	}
	const auto overlay = FindOverlay(window);
	return overlay && overlay->active();
}

void SyncPrivacyScreenForWindow(not_null<::MainWindow*> window) {
	if (!SurfaceState.privacyActive || !window->bodyWidget()) {
		return;
	}
	EnsureOverlay(window)->activate(window->bodyWidget()->rect());
}

void TogglePrivacyScreen(not_null<::MainWindow*> window) {
	if (SurfaceState.privacyActive) {
		DismissPrivacyScreen();
		return;
	}
	PrivacyAppFilter::Instance().ensurePasscodeSubscription();
	SurfaceState.onActivate(Core::App().passcodeLocked());
	SyncAppFilterInstallation();
	Core::App().closeMediaViewAndPanelsForPrivacy();
	Core::App().notifications().clearAllFast();
	HideSecondaryTopLevelSurfaces();
	Core::App().enumerateWindows([](not_null<Window::Controller*> ctrl) {
		if (const auto w = ctrl->widget(); w && w->bodyWidget()) {
			EnsureOverlay(w)->activate(w->bodyWidget()->rect());
		}
	});
	if (window->bodyWidget()) {
		EnsureOverlay(window)->activate(window->bodyWidget()->rect());
	}
	GlobalPrivacyChanges.fire(true);
	RefreshMenusAndTitles();
	Core::App().notifications().updateAll();
}

void DismissPrivacyScreen() {
	if (!SurfaceState.privacyActive) {
		return;
	}
	const auto locked = Core::App().passcodeLocked();
	const auto shouldRestoreNow = SurfaceState.onDismiss(
		locked,
		HasHiddenTopLevels());
	Core::App().enumerateWindows([](not_null<Window::Controller*> ctrl) {
		if (const auto w = ctrl->widget()) {
			if (auto *overlay = FindOverlay(w)) {
				overlay->dismissLocal();
			}
		}
	});
	if (shouldRestoreNow) {
		RestoreSecondaryTopLevelSurfaces();
	}
	SyncAppFilterInstallation();
	GlobalPrivacyChanges.fire(false);
	RefreshMenusAndTitles();
	Core::App().notifications().updateAll();
	if (!locked) {
		Core::App().checkStartUrls();
	}
}

void UpdatePrivacyScreenLayout(not_null<::MainWindow*> window, QRect body) {
	if (auto *overlay = FindOverlay(window.get()); overlay && overlay->active()) {
		overlay->setGeometry(body);
		if (!Core::App().passcodeLocked() && !window->controller().locked()) {
			overlay->raise();
		}
	}
}

void RaisePrivacyScreen(not_null<::MainWindow*> window) {
	if (auto *overlay = FindOverlay(window.get()); overlay && overlay->active()) {
		overlay->raise();
	}
}

void ShowPrivacyWeatherSettings(not_null<Window::SessionController*> controller) {
	if (!WeatherSettingsVisible()) {
		return;
	}
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Privacy screen weather"_q));
		box->addRow(object_ptr<Ui::FlatLabel>(box, rpl::single(
			u"Weather uses a rounded area sent to Open-Meteo. It stays off unless "
			"you choose a place or enable approximate location. An in-app proxy "
			"suspends these requests."_q), st::boxLabel));
#ifdef Q_OS_MAC
		box->addRow(object_ptr<Ui::FlatLabel>(box, rpl::single(
			u"Street address lookup sends the submitted address to Apple."_q),
			st::boxLabel));
		const auto placeholder = u"Town, postal code or street address"_q;
#else
		const auto placeholder = u"Town or postal code"_q;
#endif // Q_OS_MAC
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box, st::defaultInputField, rpl::single(placeholder)));
		const auto initial = (WeatherSource() == "current"
			&& WeatherStore::Instance().locationFailed())
			? u"Location is unavailable. Choose a place or permit location in system settings."_q
			: QString();
		const auto status = box->addRow(object_ptr<Ui::FlatLabel>(
			box, rpl::single(initial), st::boxLabel));
		const auto results = box->addRow(object_ptr<Ui::VerticalLayout>(box));
		const auto weak = QPointer(box.get());
		const auto generation = box->lifetime().make_state<int>(0);
		const auto accept = [=](const GeoPoint &cell) {
			if (weak && WeatherPermitted() && WeatherSettingsVisible()) {
				ChangeWeatherSource("place", cell);
				box->closeBox();
			}
		};
		const auto addResult = [=](QString title, GeoPoint cell) {
			const auto choice = results->add(object_ptr<Ui::RoundButton>(
				results, rpl::single(std::move(title)), st::defaultLightButton));
			choice->setClickedCallback([=] { accept(cell); });
		};
		const auto search = [=] {
			if (!WeatherSettingsVisible()) {
				return;
			}
			if (!WeatherPermitted()) {
				status->setText(u"Weather search is paused while the app proxy is on."_q);
				return;
			}
			const auto query = field->getLastText().trimmed();
			if (query.isEmpty()) {
				return;
			}
			const auto serial = ++*generation;
			results->clear();
			status->setText(u"Looking up places…"_q);
			WeatherStore::Instance().findPlaces(query, [=](std::vector<GeoPoint> places) {
				if (!weak || serial != *generation || !WeatherSettingsVisible()) {
					return;
				}
				status->setText(places.empty()
					? u"No places found. Try adding a country or region."_q
					: u"Choose a place to use its rounded area."_q);
				for (auto &place : places) {
					auto title = base::take(place.displayName);
					addResult(std::move(title), std::move(place));
				}
			});
		};
		field->submits() | rpl::on_next([=](Qt::KeyboardModifiers) { search(); },
			field->lifetime());
		const auto lookup = box->addRow(object_ptr<Ui::RoundButton>(
			box, rpl::single(u"Search towns"_q), st::defaultLightButton));
		lookup->setClickedCallback(search);
#ifdef Q_OS_MAC
		const auto address = box->addRow(object_ptr<Ui::RoundButton>(
			box, rpl::single(u"Resolve street address with Apple"_q),
			st::defaultLightButton));
		address->setClickedCallback([=] {
			const auto query = field->getLastText().trimmed();
			if (query.isEmpty() || !WeatherPermitted() || !WeatherSettingsVisible()) {
				return;
			}
			const auto serial = ++*generation;
			results->clear();
			status->setText(u"Apple receives the address you submit. Looking it up…"_q);
			Platform::ResolveWeatherAddress(query, [=](Core::GeoLocation location) {
				if (!weak || serial != *generation || !WeatherPermitted()
					|| !WeatherSettingsVisible()) {
					return;
				}
				const auto cell = location.exact()
					? PrivacyWeather::RoundedCell(location.point.x(), location.point.y())
					: std::nullopt;
				status->setText(cell
					? u"Confirm to use the rounded area."_q
					: u"Address unavailable. Try a town or postal code."_q);
				if (cell) {
					addResult(u"Use this area"_q, *cell);
				}
			});
		});
#endif // Q_OS_MAC
		const auto unitsText = [] {
			const auto units = WeatherPref(kWeatherUnitsPref);
			return (units == "c")
				? u"Units · Celsius"_q
				: (units == "f")
				? u"Units · Fahrenheit"_q
				: u"Units · System"_q;
		};
		const auto units = box->addRow(object_ptr<Ui::RoundButton>(box,
			rpl::single(unitsText()) | rpl::then(WeatherChanges.events() | rpl::map(unitsText)),
			st::defaultLightButton));
		units->setClickedCallback([] {
			const auto units = WeatherPref(kWeatherUnitsPref);
			Core::App().settings().writePref<QByteArray>(kWeatherUnitsPref,
				QByteArray(units == "c" ? "f" : units == "f" ? "system" : "c"));
			Core::App().saveSettingsDelayed();
			WeatherChanges.fire({});
		});
		box->addButton(rpl::single(u"Use approximate location"_q), [=] {
			if (!WeatherSettingsVisible()) {
				return;
			}
			if (!WeatherPermitted()) {
				status->setText(u"Disable the app proxy before requesting location."_q);
				return;
			}
			ChangeWeatherSource("current");
			WeatherStore::Instance().resolveLocation(true);
			box->closeBox();
		});
		box->addButton(rpl::single(u"Turn off"_q), [=] {
			ChangeWeatherSource("off");
			box->closeBox();
		});
		box->addButton(rpl::single(u"Close"_q), [=] { box->closeBox(); });
	}));
}

void BuildPrivacyScreenSetting(::Settings::Builder::SectionBuilder &builder) {
	const auto labelText = [] {
		const auto source = WeatherSource();
		return source == "current" ? u"Approximate location"_q
			: source == "place" ? u"Configured place"_q : u"Off"_q;
	};
	const auto button = builder.addButton({
		.id = u"advanced/purple_privacy_weather"_q,
		.title = rpl::single(u"Privacy screen weather"_q),
		.st = &st::settingsButtonNoIcon,
		.label = rpl::single(labelText())
			| rpl::then(WeatherChanges.events() | rpl::map(labelText)),
		.keywords = { u"privacy"_q, u"clock"_q, u"weather"_q, u"lock"_q },
	});
	if (button) {
		const auto controller = builder.controller();
		button->setClickedCallback([=] { ShowPrivacyWeatherSettings(controller); });
	}
}

} // namespace Purple

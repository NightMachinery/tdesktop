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
#include "purple/purple_privacy_canvas.h"
#include "purple/purple_privacy_clocks.h"
#include "purple/purple_privacy_state.h"
#include "purple/purple_privacy_weather.h"
#include "purple/purple_privacy_weather_location.h"
#include "purple/purple_privacy_weather_proxy.h"
#include "settings/settings_builder.h"
#include "storage/storage_domain.h"
#include "tray.h"
#include "ui/boxes/confirm_box.h"
#include "ui/effects/animations.h"
#include "ui/layers/generic_box.h"
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
using PrivacyWeather::LocationAccess;
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
	return PrivacyWeather::EffectiveSource(WeatherPref(kWeatherSourcePref),
		!WeatherPref(PrivacyWeather::kLocationPref).isEmpty());
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

bool Fahrenheit(const QLocale &locale) {
	const auto units = WeatherPref(kWeatherUnitsPref);
	return units == "f" || ((units.isEmpty() || units == "system")
		&& locale.measurementSystem() == QLocale::ImperialUSSystem);
}

QNetworkProxy WeatherProxy() {
	const auto &proxy = Core::App().settings().proxy();
	return PrivacyWeather::NetworkProxy(proxy.isSystem(), proxy.isEnabled(),
		MTP::ToNetworkProxy(proxy.selected()));
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
		return _snapshot && now - _updated < kExpireForecast
			? &*_snapshot
			: nullptr;
	}

	[[nodiscard]] QString locationStatusText() const {
		if (WeatherSource() != "current" || !_locationIssue) {
			return {};
		}
		const auto reason = [&] {
			switch (*_locationIssue) {
			case LocationAccess::Denied: return u"Location permission was denied."_q;
			case LocationAccess::Restricted: return u"Location access is restricted."_q;
			case LocationAccess::Disabled: return u"Location Services is off."_q;
			case LocationAccess::NotDetermined: return u"Location permission is still needed."_q;
			default: return u"System location is unavailable."_q;
			}
		}();
		return reason + u" You can search for a town or postal code below."_q;
	}

	[[nodiscard]] bool fresh(qint64 now) const {
		return snapshot(now) && !_failed && now - _updated < kFreshForecast;
	}

	[[nodiscard]] QString status(const QDateTime &now) const {
		if (WeatherSource() == "off") {
			return {};
		}
		const auto ms = now.toMSecsSinceEpoch();
		if (!snapshot(ms)) {
			return _failed ? u"Weather unavailable"_q : QString();
		}
		const auto updated = QLocale::system().toString(
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
				PrivacyScreenActiveAny() && !Core::App().passcodeLocked(),
				source != "off",
				_forecast != nullptr,
				_nextAttempt,
				now)) {
			return;
		}
		if (source == "current") {
			if (!_locating && now - _lastLocationAttempt >= kCurrentLocationInterval) {
				resolveLocation();
				return;
			}
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
		if (const auto reply = base::take(_forecast)) {
			reply->abort();
		}
		_currentCell.reset();
		_legacyCell.reset();
		_snapshot.reset();
		_updated = 0;
		_nextAttempt = 0;
		_lastLocationAttempt = 0;
		_locating = false;
		_failed = false;
		_locationIssue.reset();
		WeatherChanges.fire({});
	}

	void locationUnavailable(LocationAccess issue) {
		_currentCell.reset();
		_snapshot.reset();
		_nextAttempt = 0;
		_failed = false;
		_locationIssue = issue;
		WeatherChanges.fire({});
	}

	void resolveLocation() {
		_lastLocationAttempt = QDateTime::currentMSecsSinceEpoch();
#ifdef Q_OS_MAC
		const auto access = Platform::CurrentWeatherLocationAccess();
		if (!PrivacyWeather::CanRequestLocation(access, _locationPrompted)) {
			locationUnavailable(access);
			return;
		}
		const auto allowPrompt = (access == LocationAccess::NotDetermined);
		_locationPrompted |= allowPrompt;
#endif // Q_OS_MAC
		_locating = true;
		const auto generation = _generation;
		const auto done = [=](Core::GeoLocation location) {
			if (generation != _generation) {
				return;
			}
			_locating = false;
			const auto cell = location.exact()
				? PrivacyWeather::RoundedCell(location.point.x(), location.point.y())
				: std::nullopt;
			if (!cell) {
#ifdef Q_OS_MAC
				const auto access = Platform::CurrentWeatherLocationAccess();
				locationUnavailable(access == LocationAccess::Authorized
					? LocationAccess::Unavailable
					: access);
#else // Q_OS_MAC
				locationUnavailable(LocationAccess::Unavailable);
#endif // Q_OS_MAC
				return;
			}
			if (!_currentCell || CellKey(*cell) != CellKey(*_currentCell)) {
				_snapshot.reset();
				_nextAttempt = 0;
			}
			_currentCell = cell;
			_locationIssue.reset();
			WeatherChanges.fire({});
			ensureFresh();
		};
#ifdef Q_OS_MAC
		Platform::ResolveCurrentWeatherLocation(allowPrompt, done);
#else // Q_OS_MAC
		Platform::ResolveCurrentExactLocation(done);
#endif // Q_OS_MAC
	}

	void findPlaces(
			const QString &query,
			Fn<bool()> wanted,
			Fn<void(std::vector<GeoPoint>)> done) {
		if (!wanted()) {
			return;
		}
		const auto url = PrivacyWeather::BuildGeocodeUrl(query, 5);
		if (!url) {
			done({});
			return;
		}
		const auto reply = get(*url);
		connect(reply, &QNetworkReply::finished, this, [=] {
			reply->deleteLater();
			if (reply->manager() != _nam) {
				findPlaces(query, wanted, done);
				return;
			}
			if (wanted()) {
				done(reply->error() == QNetworkReply::NoError
					? PrivacyWeather::ParsePlaces(reply->readAll())
					: std::vector<GeoPoint>());
			}
		});
	}

private:
	WeatherStore() {
		Core::App().settings().proxy().connectionTypeChanges(
		) | rpl::on_next([=] {
			const auto proxy = WeatherProxy();
			if (_proxy == proxy) {
				return;
			}
			_proxy = proxy;
			const auto old = base::take(_nam);
			const auto interrupted = (base::take(_forecast) != nullptr);
			if (old) {
				for (const auto reply : old->findChildren<QNetworkReply*>()) {
					if (reply->isRunning()) {
						reply->abort();
					}
				}
				old->deleteLater();
			}
			if (interrupted || _failed) {
				_nextAttempt = 0;
			}
			ensureFresh();
		}, _lifetime);
	}

	QNetworkReply *get(const QUrl &url) {
		if (!_nam) {
			_nam = new QNetworkAccessManager(this);
			_nam->setProxy(_proxy);
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
			if (reply->error() != QNetworkReply::NoError) {
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
			if (reply->error() != QNetworkReply::NoError) {
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

	QNetworkProxy _proxy = WeatherProxy();
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
	std::optional<LocationAccess> _locationIssue;
	bool _locating = false;
#ifdef Q_OS_MAC
	bool _locationPrompted = false;
#endif // Q_OS_MAC
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
		_backdrop->setControlsTop(y);
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
: RpWidget(parent)
, _canvas(new PrivacyVisual::Canvas(this)) {
	setFocusPolicy(Qt::NoFocus);
	setAttribute(Qt::WA_TransparentForMouseEvents);
	_canvas->setFont(st::normalFont->f);

	auto *tick = new QTimer(this);
	tick->setInterval(1000);
	connect(tick, &QTimer::timeout, this, [this, last = qint64(-1)]() mutable {
		const auto minute = QDateTime::currentMSecsSinceEpoch() / 60000;
		if (isVisible() && last != minute) {
			last = minute;
			refreshCanvas();
			refreshWeatherIfConfigured();
		}
	});
	connect(qApp, &QApplication::applicationStateChanged, this, [this] {
		if (isVisible()) {
			refreshCanvas();
			refreshWeatherIfConfigured();
		}
	});
	tick->start();

	WeatherChanges.events() | rpl::on_next([this] {
		if (isVisible()) {
			refreshCanvas();
		}
	}, _weatherLifetime);
	style::PaletteChanged() | rpl::on_next([this] {
		refreshCanvas();
	}, _weatherLifetime);
}

PrivacyBackdropWidget::~PrivacyBackdropWidget() = default;

void PrivacyBackdropWidget::setInteractiveHint(const QString &hint) {
	_hint = hint;
	refreshCanvas();
}

void PrivacyBackdropWidget::setControlsTop(int top) {
	if (_controlsTop == top) {
		return;
	}
	_controlsTop = top;
	refreshCanvas();
}

void PrivacyBackdropWidget::resizeEvent(QResizeEvent *event) {
	RpWidget::resizeEvent(event);
	_canvas->setGeometry(rect());
}

void PrivacyBackdropWidget::showEvent(QShowEvent *event) {
	RpWidget::showEvent(event);
	refreshCanvas();
	refreshWeatherIfConfigured();
}

void PrivacyBackdropWidget::refreshWeatherIfConfigured() {
	if (isVisible()) {
		WeatherStore::Instance().ensureFresh();
	}
}

void PrivacyBackdropWidget::refreshCanvas() {
	const auto utc = QDateTime::currentDateTimeUtc();
	const auto zone = QTimeZone::systemTimeZone();
	const auto local = utc.toTimeZone(zone);
	const auto locale = QLocale::system();
	const auto ms = utc.toMSecsSinceEpoch();
	const auto &weather = WeatherStore::Instance();
	const auto forecast = weather.snapshot(ms);
	const auto fresh = weather.fresh(ms);
	const auto localDay = local.time().hour() >= 7 && local.time().hour() < 19;
	auto flat = anim::Disabled();
#ifdef Q_OS_MAC
	flat = flat || PrivacyVisual::PrefersFlatColors();
#endif // Q_OS_MAC
	_canvas->setFrame({
		.local = local,
		.locale = locale,
		.clocks = PrivacyClocks::At(utc, zone, ActiveSettings().worldClocks),
		.weather = forecast ? std::make_optional(*forecast) : std::nullopt,
		.update = weather.status(local),
		.hint = _hint,
		.controlsTop = _controlsTop,
		.motifCode = fresh ? forecast->code : 0,
		.dark = st::windowBg->c.lightness() < 128,
		.day = fresh ? forecast->isDay.value_or(localDay) : localDay,
		.flat = flat,
		.fahrenheit = Fahrenheit(locale),
	});
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
			u"Weather uses approximate current location by default. Open-Meteo "
			"receives the rounded area for forecasts and the text you "
			"submit for town searches. Requests use your SOCKS5 or HTTP app proxy "
			"when enabled, follow Qt in system proxy mode, and go direct otherwise."_q), st::boxLabel));
#ifdef Q_OS_MAC
		box->addRow(object_ptr<Ui::FlatLabel>(box, rpl::single(
			u"Street address lookup uses Apple’s system service and sends the "
			"submitted address to Apple outside the app proxy."_q),
			st::boxLabel));
		const auto placeholder = u"Town, postal code or street address"_q;
#else
		const auto placeholder = u"Town or postal code"_q;
#endif // Q_OS_MAC
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box, st::defaultInputField, rpl::single(placeholder)));
		const auto status = box->addRow(object_ptr<Ui::FlatLabel>(
			box, rpl::single(WeatherStore::Instance().locationStatusText()),
			st::boxLabel));
		const auto results = box->addRow(object_ptr<Ui::VerticalLayout>(box));
		const auto weak = QPointer(box.get());
		const auto generation = box->lifetime().make_state<int>(0);
		const auto accept = [=](const GeoPoint &cell) {
			if (weak && WeatherSettingsVisible()) {
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
			const auto query = field->getLastText().trimmed();
			if (query.isEmpty()) {
				return;
			}
			const auto serial = ++*generation;
			results->clear();
			status->setText(u"Looking up places…"_q);
			WeatherStore::Instance().findPlaces(query, [=] {
				return weak && serial == *generation && WeatherSettingsVisible();
			}, [=](std::vector<GeoPoint> places) {
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
			if (query.isEmpty() || !WeatherSettingsVisible()) {
				return;
			}
			const auto serial = ++*generation;
			results->clear();
			status->setText(u"Apple receives the address you submit. Looking it up…"_q);
			Platform::ResolveWeatherAddress(query, [=](Core::GeoLocation location) {
				if (!weak || serial != *generation || !WeatherSettingsVisible()) {
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
			ChangeWeatherSource("current");
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

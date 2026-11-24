#include "purple/purple_privacy_screen.h"

#include "base/weak_ptr.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/core_settings_proxy.h"
#include "main/main_domain.h"
#include "mainwindow.h"
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
#include "window/main_window.h"
#include "window/notifications_manager.h"
#include "window/window_controller.h"

#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QDateTime>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QFont>
#include <QtGui/QKeyEvent>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtWidgets/QApplication>

#include <vector>

namespace Purple {
namespace {

constexpr auto kPrivacyOverlayName = "purple-privacy-overlay";

rpl::event_stream<> WeatherChanges;
bool GlobalPrivacyActive = false;
rpl::event_stream<bool> GlobalPrivacyChanges;
std::vector<QPointer<QWidget>> HiddenTopLevelWindows;

class WeatherStore final : public QObject {
public:
	static WeatherStore &Instance() {
		static auto *instance = new WeatherStore();
		return *instance;
	}

	[[nodiscard]] QString formattedLine() const {
		const auto loc = PrivacyWeatherLocation();
		const auto clean = PrivacyWeather::SanitizeLocation(loc);
		if (clean.isEmpty() || Core::App().settings().proxy().isEnabled()) {
			return QString();
		}
		if (clean != _fetchedLocation) {
			return QString();
		}
		return PrivacyWeather::FormatWeatherLine(loc, _snapshot, _failed);
	}

	void ensureFresh() {
		const auto raw = PrivacyWeatherLocation();
		const auto loc = PrivacyWeather::SanitizeLocation(raw);
		if (loc.isEmpty()) {
			_fetchedLocation.clear();
			_snapshot.reset();
			_failed = false;
			return;
		}
		const auto now = QDateTime::currentMSecsSinceEpoch();
		const auto proxyOn = Core::App().settings().proxy().isEnabled();
		if (!PrivacyWeather::ShouldFetchWeather(
				raw,
				_fetchedLocation,
				_snapshot.has_value(),
				_failed,
				_inFlight,
				_inFlightLocation,
				_lastAttemptMs,
				now,
				proxyOn)) {
			return;
		}
		startFetch(loc);
	}

	void invalidate() {
		_fetchedLocation.clear();
		_snapshot.reset();
		_failed = false;
		_lastAttemptMs = 0;
		WeatherChanges.fire({});
	}

private:
	WeatherStore() = default;

	QNetworkRequest makeRequest(const QUrl &url) {
		auto req = QNetworkRequest(url);
		req.setRawHeader("User-Agent", PrivacyWeather::kGenericUserAgent);
		req.setRawHeader("Accept-Language", PrivacyWeather::kFixedAcceptLanguage);
		req.setAttribute(
			QNetworkRequest::CookieLoadControlAttribute,
			QNetworkRequest::Manual);
		req.setAttribute(
			QNetworkRequest::CookieSaveControlAttribute,
			QNetworkRequest::Manual);
		req.setAttribute(
			QNetworkRequest::CacheLoadControlAttribute,
			QNetworkRequest::AlwaysNetwork);
		req.setTransferTimeout(8000);
		return req;
	}

	void startFetch(const QString &location) {
		const auto url = PrivacyWeather::BuildGeocodeUrl(location);
		if (!url) {
			return;
		}
		if (!_nam) {
			_nam = new QNetworkAccessManager(this);
		}
		_inFlight = true;
		_inFlightLocation = location;
		auto *reply = _nam->get(makeRequest(*url));
		connect(reply, &QNetworkReply::finished, this, [this, reply, location] {
			reply->deleteLater();
			if (_inFlightLocation != location) {
				return;
			}
			if (reply->error() != QNetworkReply::NoError
				|| Core::App().settings().proxy().isEnabled()) {
				markFailed(location);
				return;
			}
			const auto geo = PrivacyWeather::ParseGeocodeResponse(
				reply->readAll(),
				location);
			if (!geo) {
				markFailed(location);
				return;
			}
			fetchForecast(location, *geo);
		});
	}

	void fetchForecast(
			const QString &location,
			const PrivacyWeather::GeoPoint &geo) {
		const auto url = PrivacyWeather::BuildForecastUrl(
			geo.latitude,
			geo.longitude);
		auto *reply = _nam->get(makeRequest(url));
		connect(reply, &QNetworkReply::finished, this, [this, reply, location, geo] {
			reply->deleteLater();
			if (_inFlightLocation != location) {
				return;
			}
			_inFlight = false;
			if (reply->error() != QNetworkReply::NoError
				|| Core::App().settings().proxy().isEnabled()) {
				markFailed(location);
				return;
			}
			const auto snap = PrivacyWeather::ParseForecastResponse(
				reply->readAll(),
				geo.displayName);
			if (!snap) {
				markFailed(location);
				return;
			}
			_fetchedLocation = location;
			_snapshot = snap;
			_failed = false;
			_lastAttemptMs = QDateTime::currentMSecsSinceEpoch();
			WeatherChanges.fire({});
		});
	}

	void markFailed(const QString &location) {
		_inFlight = false;
		_fetchedLocation = location;
		_snapshot.reset();
		_failed = true;
		_lastAttemptMs = QDateTime::currentMSecsSinceEpoch();
		WeatherChanges.fire({});
	}

	QNetworkAccessManager *_nam = nullptr;
	bool _inFlight = false;
	QString _inFlightLocation;
	QString _fetchedLocation;
	std::optional<PrivacyWeather::WeatherSnapshot> _snapshot;
	bool _failed = false;
	qint64 _lastAttemptMs = 0;
};

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
		_lockButton->setClickedCallback([=] {
			if (Core::App().domain().local().hasLocalPasscode()) {
				// Lock first so passcode covers every window before privacy dismisses (no chat flash).
				Core::App().lockByPasscode();
				DismissPrivacyScreen();
			}
		});
		_lockButton->hide();
		qApp->installEventFilter(this);
	}

	~PrivacyOverlayWidget() override {
		if (qApp) {
			qApp->removeEventFilter(this);
		}
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

protected:
	void resizeEvent(QResizeEvent *e) override {
		RpWidget::resizeEvent(e);
		_backdrop->setGeometry(rect());
		layoutButtons();
	}

	bool eventFilter(QObject *watched, QEvent *event) override {
		if (!_active) {
			return RpWidget::eventFilter(watched, event);
		}
		if (!watched || !watched->isWidgetType()) {
			return RpWidget::eventFilter(watched, event);
		}
		const auto widget = static_cast<QWidget*>(watched);
		if (widget->window() != _window) {
			return RpWidget::eventFilter(watched, event);
		}
		// Never block the passcode/setup-email lock screen or its boxes when locked.
		if (Core::App().passcodeLocked() || _window->controller().locked()) {
			return RpWidget::eventFilter(watched, event);
		}

		const auto type = event->type();
		const auto insideOverlay = (widget == this) || isAncestorOf(widget);

		if (type == QEvent::WindowActivate) {
			raise();
			setFocus(Qt::OtherFocusReason);
			return RpWidget::eventFilter(watched, event);
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
			return RpWidget::eventFilter(watched, event);
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
		if (!ke->isAutoRepeat()
			&& ((ke->key() == Qt::Key_Escape && !mods)
				|| (ke->key() == Qt::Key_Q && !mods))) {
			DismissPrivacyScreen();
		}
		// Swallow every keystroke while privacy screen is active.
		return true;
	}

private:
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

PrivacyOverlayWidget *FindOverlay(const ::Window::MainWindow *window) {
	if (!window || !window->bodyWidget()) {
		return nullptr;
	}
	return window->bodyWidget()->findChild<PrivacyOverlayWidget*>(
		QString::fromLatin1(kPrivacyOverlayName),
		Qt::FindDirectChildrenOnly);
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
	HiddenTopLevelWindows.clear();
	for (auto *w : QApplication::topLevelWidgets()) {
		if (!w || !w->isVisible()) {
			continue;
		}
		if (dynamic_cast<::Window::MainWindow*>(w)) {
			continue;
		}
		const auto wt = w->windowType();
		if (wt == Qt::Popup || wt == Qt::ToolTip) {
			w->hide();
			continue;
		}
		// Hide auxiliary Purple windows (call panels, group call windows, Instant View,
		// bot web apps, custom notifications) without closing or ending active calls.
		HiddenTopLevelWindows.emplace_back(w);
		w->hide();
	}
}

void RestoreSecondaryTopLevelSurfaces() {
	auto list = std::move(HiddenTopLevelWindows);
	HiddenTopLevelWindows.clear();
	for (const auto &weak : list) {
		if (weak) {
			weak->show();
		}
	}
}

void RefreshMenusAndTitles() {
	Core::App().tray().updateIconCounters();
	Core::App().updateWindowTitles();
	Core::App().enumerateWindows([](not_null<Window::Controller*> ctrl) {
		if (const auto w = ctrl->widget()) {
			w->updateGlobalMenu();
		}
	});
}

} // namespace

QString PrivacyWeatherLocation() {
	return PrivacyWeather::SanitizeLocation(QString::fromUtf8(
		Core::App().settings().readPref<QByteArray>(
			PrivacyWeather::kLocationPref)));
}

void SetPrivacyWeatherLocation(const QString &location) {
	const auto clean = PrivacyWeather::SanitizeLocation(location);
	Core::App().settings().writePref<QByteArray>(
		PrivacyWeather::kLocationPref,
		clean.toUtf8());
	WeatherStore::Instance().invalidate();
}

PrivacyBackdropWidget::PrivacyBackdropWidget(not_null<QWidget*> parent)
: RpWidget(parent) {
	setFocusPolicy(Qt::NoFocus);
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setAttribute(Qt::WA_OpaquePaintEvent);

	auto *tick = new QTimer(this);
	tick->setInterval(1000);
	connect(tick, &QTimer::timeout, this, [this] {
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

void PrivacyBackdropWidget::refreshWeatherIfConfigured() {
	if (!PrivacyWeatherLocation().isEmpty()) {
		WeatherStore::Instance().ensureFresh();
	}
}

void PrivacyBackdropWidget::paintEvent(QPaintEvent *) {
	auto p = Painter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto r = rect();

	p.fillRect(r, st::windowBg->c);

	const auto now = QDateTime::currentDateTime();
	const auto timeText = now.toString(QStringLiteral("HH:mm"));
	const auto dateText = now.toString(QStringLiteral("dddd, MMMM d, yyyy"));
	const auto weatherText = WeatherStore::Instance().formattedLine();

	const auto compact = (r.height() < 420);
	auto clockFont = st::normalFont->f;
	clockFont.setPixelSize(compact ? 44 : 68);
	clockFont.setWeight(QFont::Light);

	auto dateFont = st::normalFont->f;
	dateFont.setPixelSize(compact ? 15 : 18);
	dateFont.setWeight(QFont::Normal);

	auto weatherFont = st::normalFont->f;
	weatherFont.setPixelSize(compact ? 13 : 15);
	weatherFont.setWeight(QFont::Medium);

	const auto clockH = compact ? 52 : 78;
	const auto dateH = compact ? 24 : 28;
	const auto weatherH = weatherText.isEmpty() ? 0 : (compact ? 24 : 28);
	const auto blockH = clockH + 6 + dateH + (weatherH ? (10 + weatherH) : 0);

	auto topY = std::max(36, (r.height() - blockH) / 2 - 28);

	p.setFont(clockFont);
	p.setPen(st::windowFg);
	p.drawText(QRect(24, topY, r.width() - 48, clockH), Qt::AlignCenter, timeText);
	topY += clockH + 6;

	p.setFont(dateFont);
	p.setPen(st::windowSubTextFg);
	p.drawText(QRect(24, topY, r.width() - 48, dateH), Qt::AlignCenter, dateText);
	topY += dateH;

	if (!weatherText.isEmpty()) {
		topY += 10;
		p.setFont(weatherFont);
		p.setPen(st::windowActiveTextFg);
		p.drawText(QRect(24, topY, r.width() - 48, weatherH), Qt::AlignCenter, weatherText);
	}

	if (!_hint.isEmpty()) {
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(24, r.height() - 36, r.width() - 48, 22),
			Qt::AlignCenter,
			_hint);
	}
}

bool PrivacyScreenActiveAny() {
	return GlobalPrivacyActive;
}

rpl::producer<bool> PrivacyScreenChanges() {
	return GlobalPrivacyChanges.events();
}

bool PrivacyScreenActive(const ::Window::MainWindow *window) {
	if (GlobalPrivacyActive) {
		return true;
	}
	const auto overlay = FindOverlay(window);
	return overlay && overlay->active();
}

void SyncPrivacyScreenForWindow(not_null<::MainWindow*> window) {
	if (!GlobalPrivacyActive || !window->bodyWidget()) {
		return;
	}
	EnsureOverlay(window)->activate(window->bodyWidget()->rect());
}

void TogglePrivacyScreen(not_null<::MainWindow*> window) {
	if (GlobalPrivacyActive) {
		DismissPrivacyScreen();
		return;
	}
	GlobalPrivacyActive = true;
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
	if (!GlobalPrivacyActive) {
		return;
	}
	GlobalPrivacyActive = false;
	Core::App().enumerateWindows([](not_null<Window::Controller*> ctrl) {
		if (const auto w = ctrl->widget()) {
			if (auto *overlay = FindOverlay(w)) {
				overlay->dismissLocal();
			}
		}
	});
	RestoreSecondaryTopLevelSurfaces();
	GlobalPrivacyChanges.fire(false);
	RefreshMenusAndTitles();
	Core::App().notifications().updateAll();
	if (!Core::App().passcodeLocked()) {
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

void BuildPrivacyScreenSetting(::Settings::Builder::SectionBuilder &builder) {
	const auto labelText = [] {
		const auto loc = PrivacyWeatherLocation();
		return loc.isEmpty() ? QStringLiteral("Off") : loc;
	};
	const auto button = builder.addButton({
		.id = u"advanced/purple_privacy_weather"_q,
		.title = rpl::single(QStringLiteral("Privacy screen weather location")),
		.st = &st::settingsButtonNoIcon,
		.rightText = rpl::single(labelText())
			| rpl::then(WeatherChanges.events() | rpl::map(labelText)),
		.keywords = { u"privacy"_q, u"clock"_q, u"weather"_q, u"lock"_q },
	});
	if (button) {
		const auto controller = builder.controller();
		button->setClickedCallback([=] {
			controller->show(Box([=](not_null<Ui::GenericBox*> box) {
				box->setTitle(rpl::single(QStringLiteral("Privacy screen weather")));
				box->addRow(object_ptr<Ui::FlatLabel>(
					box,
					rpl::single(QStringLiteral(
						"Optional city name for weather on the privacy screen. "
						"Leave blank to make zero weather network requests. "
						"When set, the city name and your IP address are sent directly to Open-Meteo; "
						"requests are skipped automatically while an in-app proxy is enabled.")),
					st::boxLabel));
				const auto field = box->addRow(object_ptr<Ui::InputField>(
					box,
					st::defaultInputField,
					rpl::single(QStringLiteral("City (optional, e.g. London)")),
					PrivacyWeatherLocation()));
				const auto save = [=] {
					SetPrivacyWeatherLocation(field->getLastText());
					box->closeBox();
				};
				field->submits() | rpl::on_next([=](Qt::KeyboardModifiers) {
					save();
				}, field->lifetime());
				box->setFocusCallback([=] { field->setFocusFast(); });
				box->addButton(rpl::single(QStringLiteral("Save")), save);
				box->addButton(rpl::single(QStringLiteral("Cancel")), [=] {
					box->closeBox();
				});
			}));
		});
	}
}

} // namespace Purple

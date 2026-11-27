#include "purple/purple_privacy_clocks.h"
#include "purple/purple_privacy_state.h"
#include "purple/purple_privacy_weather.h"
#include "purple/purple_privacy_weather_location.h"
#include "purple/purple_privacy_weather_proxy.h"

#include <iostream>
#include <string>

namespace {

using namespace Purple::PrivacyWeather;
using namespace Purple::PrivacyState;

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string &name) {
	++gChecks;
	if (!ok) {
		++gFailures;
		std::cerr << "FAIL: " << name << "\n";
	}
}

} // namespace

int main() {
	// 1. Empty / whitespace location produces zero network requests.
	Check(!BuildGeocodeUrl(QString()).has_value(), "empty location -> no geocode URL");
	Check(!BuildGeocodeUrl(QStringLiteral("   \t\n  ")).has_value(), "whitespace location -> no geocode URL");


	// 2. Fixed generic User-Agent and Accept-Language contain no app-identifying details.
	const auto ua = QString::fromLatin1(kGenericUserAgent);
	Check(ua == QStringLiteral("ClockWidget/1.0"), "fixed generic User-Agent");
	Check(!ua.contains(QStringLiteral("Telegram"), Qt::CaseInsensitive), "User-Agent omits Telegram");
	Check(!ua.contains(QStringLiteral("Purple"), Qt::CaseInsensitive), "User-Agent omits Purple");
	Check(QString::fromLatin1(kFixedAcceptLanguage) == QStringLiteral("en-US,en;q=0.9"),
		"fixed constant Accept-Language");

	// 3. Valid location builds HTTPS Open-Meteo URLs.
	const auto geoUrl = BuildGeocodeUrl(QStringLiteral("  London  "));
	Check(geoUrl.has_value() && geoUrl->scheme() == QStringLiteral("https"), "HTTPS geocode URL");
	Check(geoUrl->toString().contains(QStringLiteral("name=London")), "trimmed location query");

	const auto forecastUrl = BuildForecastUrl(51.5072, -0.1276);
	Check(forecastUrl.scheme() == QStringLiteral("https"), "HTTPS forecast URL");
	Check(forecastUrl.toString().contains(QStringLiteral("latitude=51.5")), "latitude rounded to a tenth of a degree");

	// 4. Geocode & forecast JSON parsing.
	const auto geo = ParseGeocodeResponse(
		R"({"results":[{"name":"London","latitude":51.5072,"longitude":-0.1276}]})",
		QStringLiteral("london"));
	Check(geo.has_value() && geo->displayName == QStringLiteral("London"), "parses valid geocode");

	const auto badGeo = ParseGeocodeResponse(R"({"results":[]})", QStringLiteral("nowhere"));
	Check(!badGeo.has_value(), "rejects empty geocode results");

	const auto snap = ParseForecastResponse(
		R"({"current":{"temperature_2m":14.6,"weather_code":2}})",
		QStringLiteral("London"));
	Check(snap.has_value() && snap->valid && snap->temperatureC == 15
		&& snap->condition == QStringLiteral("Partly cloudy"),
		"parses forecast and WMO code");

	const auto badSnap = ParseForecastResponse(R"(not-json)", QStringLiteral("London"));
	Check(!badSnap.has_value(), "rejects malformed forecast JSON");

	{
		Check(EffectiveSource({}, std::nullopt) == "current",
			"untouched settings use approximate location");
		Check(EffectiveSource({}, QByteArray("Oslo")) == "place",
			"legacy town stays selected");
		Check(EffectiveSource({}, QByteArray()) == "off",
			"a saved blank legacy town keeps weather off");
		Check(EffectiveSource("off", QByteArray("Oslo")) == "off",
			"explicit off wins even with a legacy town");
		Check(EffectiveSource("off", std::nullopt) == "off",
			"explicit off survives without a saved location");
		Check(EffectiveSource("place", std::nullopt) == "place",
			"explicit town stays selected");
		Check(EffectiveSource("current", QByteArray()) == "current",
			"explicit approximate location wins over a saved blank town");
		Check(CanRequestLocation(LocationAccess::NotDetermined, false),
			"undetermined permission may prompt on the first due cover");
		Check(!CanRequestLocation(LocationAccess::NotDetermined, true),
			"undetermined permission cannot prompt again in this session");
		Check(CanRequestLocation(LocationAccess::Authorized, true),
			"once authorized, location refresh remains available");
		for (const auto access : { LocationAccess::Denied, LocationAccess::Restricted,
				LocationAccess::Disabled, LocationAccess::Unavailable }) {
			Check(!CanRequestLocation(access, false),
				"blocked access cannot start location or request permission");
		}
	}

	{
		const auto socks = QNetworkProxy(QNetworkProxy::Socks5Proxy,
			QStringLiteral("socks.invalid"), 1080, QStringLiteral("fixture"),
			QStringLiteral("socks password"));
		const auto http = QNetworkProxy(QNetworkProxy::HttpProxy,
			QStringLiteral("http.invalid"), 8080, QStringLiteral("fixture"),
			QStringLiteral("http password"));
		Check(NetworkProxy(false, true, socks) == socks,
			"SOCKS5 uses the selected endpoint and credentials");
		Check(NetworkProxy(false, true, http) == http,
			"HTTP uses the selected endpoint and credentials");
		Check(NetworkProxy(true, false, socks).type() == QNetworkProxy::DefaultProxy,
			"system mode delegates to Qt even with a saved SOCKS5 selection");
		Check(NetworkProxy(false, false, socks).type() == QNetworkProxy::NoProxy,
			"disabled app proxy goes direct");
		Check(NetworkProxy(false, true, QNetworkProxy::NoProxy).type()
			== QNetworkProxy::NoProxy, "MTProto and web proxies go direct");
		Check(NetworkProxy(false, true, QNetworkProxy::DefaultProxy).type()
			== QNetworkProxy::NoProxy, "empty selection stays direct");
	}

	{
		const auto now = qint64(1'000'000'000);
		Check(!CanFetchForecast(true, false, false, 0, now),
			"off never fetches even with the cover shown");
		Check(!CanFetchForecast(false, true, false, 0, now),
			"hidden cover never fetches");
		Check(!CanFetchForecast(true, true, true, 0, now),
			"one in-flight request across covers");
		Check(!CanFetchForecast(true, true, false, now + 1, now),
			"waits for backoff or refresh deadline");
		Check(CanFetchForecast(true, true, false, now, now),
			"deadline is inclusive");
	}

	{
		const auto weekend = QDateTime::fromString(
			QStringLiteral("2025-03-30T00:30:00Z"), Qt::ISODate);
		const auto utc = QTimeZone(QByteArray("UTC"));
		const auto east = QTimeZone(QByteArray("Asia/Tehran"));
		const auto configured = std::vector<Purple::WorldClock>{
			{ QStringLiteral("Asia/Tehran"), QStringLiteral("Tehran") },
			{ QStringLiteral("Asia/Kathmandu"), QStringLiteral("Kathmandu") },
			{ QStringLiteral("Pacific/Honolulu"), {} },
			{ QStringLiteral("Europe/London"), {} },
		};
		const auto clocks = Purple::PrivacyClocks::At(weekend, utc, configured);
		Check(clocks.size() == 3 && clocks[1].time.offsetFromUtc() == 20700,
			"duplicate UTC offset omitted, quarter-hour zone retained");
		Check(clocks[2].dayDifference == -1, "date is derived in each zone");
		Check(Purple::PrivacyClocks::At(weekend, east, configured).size() == 3,
			"Tehran disappears when it is the system zone");
		Check(Purple::PrivacyClocks::At(weekend.addSecs(7200), utc, configured).size() == 4,
			"DST can reveal a previously deduplicated zone");
	}
	{
		const auto cell = RoundedCell(35.6892, 51.3890);
		Check(cell && cell->latitude == 35.7 && cell->longitude == 51.4,
			"approximate location drops sub-cell precision");
		Check(!RoundedCell(91., 51.), "out-of-range coordinates refused");
		const auto snapshot = ParseForecastResponse(R"({
			"timezone":"Asia/Tehran", "current":{"temperature_2m":18, "is_day":1},
			"hourly":{"time":[1743206400], "temperature_2m":[20], "weather_code":[1],
				"precipitation_probability":[25]},
			"daily":{"time":[1743193800], "temperature_2m_min":[12], "temperature_2m_max":[22],
				"weather_code":[2]}
		})", {});
		Check(snapshot && snapshot->isDay.value_or(false)
			&& snapshot->hours.size() == 1 && snapshot->days.size() == 1
			&& snapshot->hours.front().precipitation == 25,
			"current, hourly and daily values retain their forecast zone");
		const auto partial = ParseForecastResponse(R"({
			"timezone":"UTC", "current":{"temperature_2m":18},
			"hourly":{"time":[1743206400], "temperature_2m":[20,21]}
		})", {});
		Check(partial && partial->hours.empty(), "malformed hourly arrays leave current weather usable");
		const auto sparse = ParseForecastResponse(R"({
			"timezone":"UTC", "current":{"temperature_2m":18},
			"hourly":{"time":[1743206400,1743210000], "temperature_2m":[null,20],
				"weather_code":[61,1], "precipitation_probability":[null,0]},
			"daily":{"time":[1743206400,1743292800], "temperature_2m_min":[null,25],
				"temperature_2m_max":[22,17], "weather_code":[2,3]}
		})", {});
		Check(sparse && sparse->hours.size() == 2 && !sparse->hours[0].temperatureC
			&& !sparse->hours[0].precipitation && sparse->hours[0].code == 61
			&& sparse->hours[1].precipitation == 0,
			"missing temperatures and chances stay unknown, measured zero stays zero");
		Check(sparse && sparse->days.size() == 1 && !sparse->days[0].lowC
			&& sparse->days[0].highC == 22,
			"partial daily ranges stay visible and inverted ranges are omitted");
	}

	// 7. Top-level surface classification.
	Check(ClassifyTopLevelSurface(false, false, Qt::Widget) == TopLevelAction::Ignore,
		"non-window child widgets are ignored");
	Check(ClassifyTopLevelSurface(true, true, Qt::Window) == TopLevelAction::Ignore,
		"MainWindow instances are covered by overlay, not hidden as auxiliary");
	Check(ClassifyTopLevelSurface(true, false, Qt::Popup) == TopLevelAction::HideEphemeral,
		"popups are hidden without recording for restore");
	Check(ClassifyTopLevelSurface(true, false, Qt::ToolTip) == TopLevelAction::HideEphemeral,
		"tooltips are hidden without recording for restore");
	Check(ClassifyTopLevelSurface(true, false, Qt::Window) == TopLevelAction::HideAndRecordRestore,
		"auxiliary windows (call panels, Instant View, web apps) are hidden and recorded");
	Check(ClassifyTopLevelSurface(true, false, Qt::Dialog) == TopLevelAction::HideAndRecordRestore,
		"auxiliary dialogs are hidden and recorded");

	Check(CanRestoreFocus(false, true, true, true), "uncover restores the saved focus");
	Check(!CanRestoreFocus(true, true, true, true), "lock prevents focus restore");
	Check(!CanRestoreFocus(false, false, true, true), "changed session prevents focus restore");
	Check(!CanRestoreFocus(false, true, false, true), "foreign window prevents focus restore");
	Check(!CanRestoreFocus(false, true, true, false), "destroyed or hidden field cannot regain focus");

	// 8. SurfaceTracker: normal activate -> dismiss restores immediately.
	{
		SurfaceTracker st;
		Check(!st.needsAppFilter(), "idle tracker needs no app filter");
		st.onActivate(false);
		Check(st.needsAppFilter() && st.shouldSuppressNewSurfaces(),
			"active privacy installs filter and suppresses new top-levels");
		Check(st.onDismiss(false, true), "unlocked dismiss restores hidden surfaces immediately");
		Check(!st.needsAppFilter(), "unlocked dismiss uninstalls app filter");
	}

	// 9. SurfaceTracker: Lock with passcode (lockByPasscode -> DismissPrivacyScreen) keeps
	// hidden IV/web app/call windows hidden until passcode unlocks.
	{
		SurfaceTracker st;
		st.onActivate(false);
		Check(!st.onPasscodeLockChanged(true, true), "locking while active does not restore");
		Check(!st.onDismiss(true, true), "dismissing while locked defers restore until unlock");
		Check(st.shouldKeepRecordedHidden() && st.needsAppFilter(),
			"recorded hidden surfaces remain suppressed and filter stays installed while locked");
		Check(!st.shouldSuppressNewSurfaces(),
			"after privacy dismisses onto lock, new-surface suppression flag is false");
		Check(st.onPasscodeLockChanged(false, true),
			"unlocking passcode restores deferred hidden surfaces");
		Check(!st.needsAppFilter(), "after unlock restore, app filter is uninstalled");
	}

	// 10. SurfaceTracker: Lock with passcode when NO auxiliary windows were hidden uninstalls filter cleanly.
	{
		SurfaceTracker st;
		st.onActivate(false);
		Check(!st.onPasscodeLockChanged(true, false), "locking with no hidden windows");
		Check(!st.onDismiss(true, false), "dismissing while locked with no hidden windows");
		Check(!st.needsAppFilter(), "no hidden surfaces -> no filter needed while locked");
		Check(!st.onPasscodeLockChanged(false, false), "unlocking has nothing to restore");
	}

	// 11. SurfaceTracker: auto-lock while privacy active, then unlock while privacy still active,
	// then dismiss privacy -> surfaces stay hidden until BOTH unlocked and privacy dismissed.
	{
		SurfaceTracker st;
		st.onActivate(false);
		Check(!st.onPasscodeLockChanged(true, true), "auto-lock while privacy active keeps surfaces hidden");
		Check(st.shouldSuppressNewSurfaces() && st.shouldKeepRecordedHidden(),
			"incoming call/IV still caught while privacy active during auto-lock");
		Check(!st.onPasscodeLockChanged(false, true),
			"unlocking while privacy still active does NOT restore yet");
		Check(st.needsAppFilter(), "filter remains installed while privacy still active");
		Check(st.onDismiss(false, true), "dismissing privacy after unlock restores surfaces");
		Check(!st.needsAppFilter(), "filter removed after full dismiss");
	}

	std::cout << gChecks << " checks, " << gFailures << " failures\n";
	return gFailures ? 1 : 0;
}

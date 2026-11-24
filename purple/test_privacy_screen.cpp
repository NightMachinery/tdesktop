#include "purple/purple_privacy_weather.h"

#include <iostream>
#include <string>

namespace {

using namespace Purple::PrivacyWeather;

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
	Check(FormatWeatherLine(QString(), std::nullopt, true).isEmpty(),
		"empty location renders nothing even on failure");

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
	Check(forecastUrl.toString().contains(QStringLiteral("latitude=51.5072")), "latitude encoded");

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

	// 5. Safe display formatting (failures leak nothing private).
	const auto okLine = FormatWeatherLine(QStringLiteral("London"), snap, false);
	Check(okLine == QStringLiteral("London  ·  15°C  ·  Partly cloudy"), "formatted weather line");

	const auto failLine = FormatWeatherLine(QStringLiteral("London"), std::nullopt, true);
	Check(failLine == QStringLiteral("Weather unavailable"), "failure shows generic non-private line");

	// 6. Fetch policy: failure backoff, refresh cadence, in-flight dedup, location change, and proxy gate.
	const qint64 t0 = 1'000'000'000;
	Check(!ShouldFetchWeather(QString(), QString(), false, false, false, QString(), 0, t0, false),
		"blank location never fetches");
	Check(!ShouldFetchWeather(QStringLiteral("London"), QString(), false, false, false, QString(), 0, t0, true),
		"in-app proxy enabled skips direct weather fetch");
	Check(ShouldFetchWeather(QStringLiteral("London"), QString(), false, false, false, QString(), 0, t0, false),
		"initial valid location fetches when no proxy");
	Check(!ShouldFetchWeather(QStringLiteral("London"), QString(), false, false, true, QStringLiteral("London"), 0, t0, false),
		"in-flight fetch for same location is not duplicated");
	Check(!ShouldFetchWeather(QStringLiteral("London"), QStringLiteral("London"), false, true, false, QString(), t0, t0 + 1000, false),
		"1s after failure does NOT retry (prevents retry storm)");
	Check(!ShouldFetchWeather(QStringLiteral("London"), QStringLiteral("London"), false, true, false, QString(), t0, t0 + kFailureBackoffMs - 1, false),
		"within failure backoff window does NOT retry");
	Check(ShouldFetchWeather(QStringLiteral("London"), QStringLiteral("London"), false, true, false, QString(), t0, t0 + kFailureBackoffMs, false),
		"at failure backoff expiry retries once");
	Check(!ShouldFetchWeather(QStringLiteral("London"), QStringLiteral("London"), true, false, false, QString(), t0, t0 + kRefreshIntervalMs - 1, false),
		"fresh snapshot within refresh interval skips fetch");
	Check(ShouldFetchWeather(QStringLiteral("London"), QStringLiteral("London"), true, false, false, QString(), t0, t0 + kRefreshIntervalMs, false),
		"stale snapshot at refresh interval refetches");
	Check(ShouldFetchWeather(QStringLiteral("Tokyo"), QStringLiteral("London"), false, true, false, QString(), t0, t0 + 1000, false),
		"changing configured location fetches immediately despite previous city failure");

	std::cout << gChecks << " checks, " << gFailures << " failures\n";
	return gFailures ? 1 : 0;
}

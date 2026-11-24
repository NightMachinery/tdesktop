#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>

#include <cmath>
#include <optional>

namespace Purple::PrivacyWeather {

inline constexpr auto kGenericUserAgent = "ClockWidget/1.0";
inline constexpr auto kFixedAcceptLanguage = "en-US,en;q=0.9";
inline constexpr auto kLocationPref = "purple-privacy-weather-location";
inline constexpr qint64 kRefreshIntervalMs = 30 * 60 * 1000;
inline constexpr qint64 kFailureBackoffMs = 15 * 60 * 1000;

struct GeoPoint {
	double latitude = 0.0;
	double longitude = 0.0;
	QString displayName;
};

struct WeatherSnapshot {
	QString location;
	int temperatureC = 0;
	QString condition;
	bool valid = false;
};

[[nodiscard]] inline QString SanitizeLocation(const QString &raw) {
	auto out = raw.trimmed();
	if (out.size() > 80) {
		out = out.left(80).trimmed();
	}
	return out;
}

[[nodiscard]] inline std::optional<QUrl> BuildGeocodeUrl(const QString &rawLocation) {
	const auto location = SanitizeLocation(rawLocation);
	if (location.isEmpty()) {
		return std::nullopt;
	}
	auto url = QUrl(QStringLiteral("https://geocoding-api.open-meteo.com/v1/search"));
	auto query = QUrlQuery();
	query.addQueryItem(QStringLiteral("name"), location);
	query.addQueryItem(QStringLiteral("count"), QStringLiteral("1"));
	query.addQueryItem(QStringLiteral("language"), QStringLiteral("en"));
	query.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
	url.setQuery(query);
	return url;
}

[[nodiscard]] inline QUrl BuildForecastUrl(double latitude, double longitude) {
	auto url = QUrl(QStringLiteral("https://api.open-meteo.com/v1/forecast"));
	auto query = QUrlQuery();
	query.addQueryItem(QStringLiteral("latitude"), QString::number(latitude, 'f', 4));
	query.addQueryItem(QStringLiteral("longitude"), QString::number(longitude, 'f', 4));
	query.addQueryItem(QStringLiteral("current"), QStringLiteral("temperature_2m,weather_code"));
	query.addQueryItem(QStringLiteral("timezone"), QStringLiteral("auto"));
	url.setQuery(query);
	return url;
}

[[nodiscard]] inline QString DescribeWmoCode(int code) {
	switch (code) {
	case 0: return QStringLiteral("Clear");
	case 1: return QStringLiteral("Mostly clear");
	case 2: return QStringLiteral("Partly cloudy");
	case 3: return QStringLiteral("Overcast");
	case 45:
	case 48: return QStringLiteral("Fog");
	case 51:
	case 53:
	case 55: return QStringLiteral("Drizzle");
	case 56:
	case 57: return QStringLiteral("Freezing drizzle");
	case 61:
	case 63:
	case 65: return QStringLiteral("Rain");
	case 66:
	case 67: return QStringLiteral("Freezing rain");
	case 71:
	case 73:
	case 75:
	case 77: return QStringLiteral("Snow");
	case 80:
	case 81:
	case 82: return QStringLiteral("Showers");
	case 85:
	case 86: return QStringLiteral("Snow showers");
	case 95:
	case 96:
	case 99: return QStringLiteral("Thunderstorm");
	default: return QStringLiteral("Weather");
	}
}

[[nodiscard]] inline std::optional<GeoPoint> ParseGeocodeResponse(
		const QByteArray &jsonBytes,
		const QString &fallbackName) {
	auto err = QJsonParseError();
	const auto doc = QJsonDocument::fromJson(jsonBytes, &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject()) {
		return std::nullopt;
	}
	const auto results = doc.object().value(QStringLiteral("results")).toArray();
	if (results.isEmpty() || !results.first().isObject()) {
		return std::nullopt;
	}
	const auto first = results.first().toObject();
	if (!first.contains(QStringLiteral("latitude"))
		|| !first.contains(QStringLiteral("longitude"))) {
		return std::nullopt;
	}
	const auto lat = first.value(QStringLiteral("latitude")).toDouble(999.0);
	const auto lon = first.value(QStringLiteral("longitude")).toDouble(999.0);
	if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
		return std::nullopt;
	}
	const auto name = first.value(QStringLiteral("name")).toString().trimmed();
	return GeoPoint{
		.latitude = lat,
		.longitude = lon,
		.displayName = name.isEmpty() ? SanitizeLocation(fallbackName) : name,
	};
}

[[nodiscard]] inline std::optional<WeatherSnapshot> ParseForecastResponse(
		const QByteArray &jsonBytes,
		const QString &locationName) {
	auto err = QJsonParseError();
	const auto doc = QJsonDocument::fromJson(jsonBytes, &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject()) {
		return std::nullopt;
	}
	const auto current = doc.object().value(QStringLiteral("current")).toObject();
	if (!current.contains(QStringLiteral("temperature_2m"))) {
		return std::nullopt;
	}
	const auto temp = current.value(QStringLiteral("temperature_2m")).toDouble(9999.0);
	if (temp < -100.0 || temp > 100.0) {
		return std::nullopt;
	}
	const auto code = current.value(QStringLiteral("weather_code")).toInt(-1);
	return WeatherSnapshot{
		.location = SanitizeLocation(locationName),
		.temperatureC = int(std::lround(temp)),
		.condition = DescribeWmoCode(code),
		.valid = true,
	};
}

[[nodiscard]] inline bool ShouldFetchWeather(
		const QString &configuredLocation,
		const QString &fetchedLocation,
		bool hasSnapshot,
		bool lastFailed,
		bool inFlight,
		const QString &inFlightLocation,
		qint64 lastAttemptMs,
		qint64 nowMs,
		bool appProxyEnabled) {
	if (appProxyEnabled) {
		return false;
	}
	const auto loc = SanitizeLocation(configuredLocation);
	if (loc.isEmpty()) {
		return false;
	}
	if (inFlight && inFlightLocation == loc) {
		return false;
	}
	if (loc == fetchedLocation) {
		const auto elapsed = (nowMs >= lastAttemptMs) ? (nowMs - lastAttemptMs) : qint64(0);
		if (hasSnapshot && elapsed < kRefreshIntervalMs) {
			return false;
		}
		if (lastFailed && elapsed < kFailureBackoffMs) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] inline QString FormatWeatherLine(
		const QString &configuredLocation,
		const std::optional<WeatherSnapshot> &snapshot,
		bool failed) {
	if (SanitizeLocation(configuredLocation).isEmpty()) {
		return QString();
	}
	if (snapshot && snapshot->valid) {
		return snapshot->location
			+ QStringLiteral("  ·  ")
			+ QString::number(snapshot->temperatureC)
			+ QStringLiteral("°C  ·  ")
			+ snapshot->condition;
	}
	if (failed) {
		return QStringLiteral("Weather unavailable");
	}
	return QString();
}

} // namespace Purple::PrivacyWeather

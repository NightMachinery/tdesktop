#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QTimeZone>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>

#include <cmath>
#include <optional>
#include <vector>

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

struct HourlyWeather {
	QDateTime time;
	std::optional<int> temperatureC;
	std::optional<int> precipitation;
	QString condition;
	int code = -1;
};

struct DailyWeather {
	QDate date;
	std::optional<int> lowC;
	std::optional<int> highC;
	QString condition;
	int code = -1;
};

struct WeatherSnapshot {
	QString location;
	int temperatureC = 0;
	QString condition;
	bool valid = false;
	std::optional<bool> isDay;
	QTimeZone zone;
	std::vector<HourlyWeather> hours;
	std::vector<DailyWeather> days;
	int code = -1;
};

[[nodiscard]] inline QString SanitizeLocation(const QString &raw) {
	auto out = raw.trimmed();
	if (out.size() > 80) {
		out = out.left(80).trimmed();
	}
	return out;
}

[[nodiscard]] inline std::optional<QUrl> BuildGeocodeUrl(const QString &rawLocation, int count = 1) {
	const auto location = SanitizeLocation(rawLocation);
	if (location.isEmpty()) {
		return std::nullopt;
	}
	auto url = QUrl(QStringLiteral("https://geocoding-api.open-meteo.com/v1/search"));
	auto query = QUrlQuery();
	query.addQueryItem(QStringLiteral("name"), location);
	query.addQueryItem(QStringLiteral("count"), QString::number(count));
	query.addQueryItem(QStringLiteral("language"), QStringLiteral("en"));
	query.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
	url.setQuery(query);
	return url;
}

[[nodiscard]] inline QUrl BuildForecastUrl(double latitude, double longitude) {
	auto url = QUrl(QStringLiteral("https://api.open-meteo.com/v1/forecast"));
	auto query = QUrlQuery();
	query.addQueryItem(QStringLiteral("latitude"), QString::number(latitude, 'f', 1));
	query.addQueryItem(QStringLiteral("longitude"), QString::number(longitude, 'f', 1));
	query.addQueryItem(QStringLiteral("current"), QStringLiteral("temperature_2m,weather_code,is_day"));
	query.addQueryItem(QStringLiteral("hourly"),
		QStringLiteral("temperature_2m,weather_code,precipitation_probability"));
	query.addQueryItem(QStringLiteral("daily"),
		QStringLiteral("weather_code,temperature_2m_min,temperature_2m_max"));
	query.addQueryItem(QStringLiteral("forecast_days"), QStringLiteral("4"));
	query.addQueryItem(QStringLiteral("timeformat"), QStringLiteral("unixtime"));
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

[[nodiscard]] inline std::optional<GeoPoint> RoundedCell(
		double latitude,
		double longitude) {
	if (!std::isfinite(latitude) || !std::isfinite(longitude)
		|| std::abs(latitude) > 90. || std::abs(longitude) > 180.) {
		return std::nullopt;
	}
	return GeoPoint{
		.latitude = std::round(latitude * 10.) / 10.,
		.longitude = std::round(longitude * 10.) / 10.,
		.displayName = {},
	};
}

[[nodiscard]] inline std::vector<GeoPoint> ParsePlaces(const QByteArray &jsonBytes) {
	const auto items = QJsonDocument::fromJson(jsonBytes).object(
	).value(QStringLiteral("results")).toArray();
	auto result = std::vector<GeoPoint>();
	for (const auto &item : items) {
		const auto fields = item.toObject();
		auto point = RoundedCell(
			fields.value(QStringLiteral("latitude")).toDouble(999.),
			fields.value(QStringLiteral("longitude")).toDouble(999.));
		if (!point) {
			continue;
		}
		auto parts = QStringList();
		for (const auto key : { "name", "admin1", "country" }) {
			const auto value = fields.value(QString::fromLatin1(key)).toString().simplified();
			if (!value.isEmpty() && !parts.contains(value)) {
				parts.push_back(value);
			}
		}
		point->displayName = parts.join(QStringLiteral(", "));
		result.push_back(std::move(*point));
	}
	return result;
}

[[nodiscard]] inline std::optional<GeoPoint> ParseGeocodeResponse(
		const QByteArray &jsonBytes,
		const QString &fallbackName) {
	auto places = ParsePlaces(jsonBytes);
	if (places.empty()) {
		return std::nullopt;
	}
	auto &point = places.front();
	const auto name = QJsonDocument::fromJson(jsonBytes).object()
		.value(QStringLiteral("results")).toArray().first().toObject()
		.value(QStringLiteral("name")).toString().trimmed();
	point.displayName = name.isEmpty() ? SanitizeLocation(fallbackName) : name;
	return std::move(point);
}

[[nodiscard]] inline std::optional<int> Temperature(const QJsonValue &value) {
	const auto temp = value.toDouble(999.);
	return (std::isfinite(temp) && std::abs(temp) <= 100.)
		? std::optional<int>(int(std::lround(temp)))
		: std::nullopt;
}

[[nodiscard]] inline std::optional<WeatherSnapshot> ParseForecastResponse(
		const QByteArray &jsonBytes,
		const QString &locationName) {
	auto err = QJsonParseError();
	const auto doc = QJsonDocument::fromJson(jsonBytes, &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject()) {
		return std::nullopt;
	}
	const auto root = doc.object();
	const auto current = root.value(QStringLiteral("current")).toObject();
	const auto temp = Temperature(current.value(QStringLiteral("temperature_2m")));
	if (!temp) {
		return std::nullopt;
	}
	const auto day = current.value(QStringLiteral("is_day")).toInt(-1);
	const auto code = current.value(QStringLiteral("weather_code")).toInt(-1);
	auto result = WeatherSnapshot{
		.location = SanitizeLocation(locationName),
		.temperatureC = *temp,
		.condition = DescribeWmoCode(code),
		.valid = true,
		.isDay = (day >= 0) ? std::optional<bool>(day != 0) : std::nullopt,
		.zone = QTimeZone(root.value(QStringLiteral("timezone")).toString().toUtf8()),
		.hours = {},
		.days = {},
		.code = code,
	};
	if (!result.zone.isValid()) {
		return result;
	}

	const auto hours = root.value(QStringLiteral("hourly")).toObject();
	const auto hourTimes = hours.value(QStringLiteral("time")).toArray();
	const auto hourTemps = hours.value(QStringLiteral("temperature_2m")).toArray();
	const auto hourCodes = hours.value(QStringLiteral("weather_code")).toArray();
	const auto hourRain = hours.value(QStringLiteral("precipitation_probability")).toArray();
	if (hourTimes.size() == hourTemps.size() && hourTimes.size() == hourCodes.size()
		&& hourTimes.size() == hourRain.size()) {
		for (auto i = qsizetype(0); i < hourTimes.size(); ++i) {
			const auto seconds = hourTimes[i].toInteger();
			const auto temperature = Temperature(hourTemps[i]);
			const auto rain = hourRain[i].toInt(-1);
			const auto code = hourCodes[i].toInt(-1);
			if (seconds > 0) {
				result.hours.push_back({
					QDateTime::fromSecsSinceEpoch(seconds, result.zone),
					temperature,
					(rain >= 0 && rain <= 100) ? std::make_optional(rain) : std::nullopt,
					DescribeWmoCode(code),
					code,
				});
			}
		}
	}

	const auto apiOffset = root.value(QStringLiteral("utc_offset_seconds")).toInt(25 * 3600);
	const auto dailyZone = std::abs(apiOffset) <= 24 * 3600
		? QTimeZone::fromSecondsAheadOfUtc(apiOffset)
		: result.zone;
	const auto days = root.value(QStringLiteral("daily")).toObject();
	const auto dayTimes = days.value(QStringLiteral("time")).toArray();
	const auto lows = days.value(QStringLiteral("temperature_2m_min")).toArray();
	const auto highs = days.value(QStringLiteral("temperature_2m_max")).toArray();
	const auto dayCodes = days.value(QStringLiteral("weather_code")).toArray();
	if (dayTimes.size() == lows.size() && dayTimes.size() == highs.size()
		&& dayTimes.size() == dayCodes.size()) {
		for (auto i = qsizetype(0); i < dayTimes.size(); ++i) {
			const auto seconds = dayTimes[i].toInteger();
			const auto low = Temperature(lows[i]);
			const auto high = Temperature(highs[i]);
			const auto code = dayCodes[i].toInt(-1);
			if (seconds > 0 && (!low || !high || *low <= *high)) {
				result.days.push_back({
					QDateTime::fromSecsSinceEpoch(seconds, dailyZone).date(),
					low,
					high,
					DescribeWmoCode(code),
					code,
				});
			}
		}
	}
	return result;
}

[[nodiscard]] inline bool CanFetchForecast(
		bool coverVisible,
		bool enabled,
		bool requestActive,
		qint64 nextAttempt,
		qint64 now) {
	return coverVisible && enabled && !requestActive
		&& now >= nextAttempt;
}

} // namespace Purple::PrivacyWeather

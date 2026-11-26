/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Core {
struct GeoLocation;
struct GeoAddress;
} // namespace Core

#ifdef Q_OS_MAC
namespace Purple::PrivacyWeather {
enum class LocationAccess;
} // namespace Purple::PrivacyWeather
#endif // Q_OS_MAC

namespace Platform {

void ResolveCurrentExactLocation(Fn<void(Core::GeoLocation)> callback);
#ifdef Q_OS_MAC
[[nodiscard]] auto CurrentWeatherLocationAccess()
-> Purple::PrivacyWeather::LocationAccess;
void ResolveCurrentWeatherLocation(
	bool allowPrompt,
	Fn<void(Core::GeoLocation)> callback);
void ResolveWeatherAddress(
	const QString &address,
	Fn<void(Core::GeoLocation)> callback);
#endif // Q_OS_MAC
void ResolveLocationAddress(
	const Core::GeoLocation &location,
	const QString &language,
	Fn<void(Core::GeoAddress)> callback);

} // namespace Platform

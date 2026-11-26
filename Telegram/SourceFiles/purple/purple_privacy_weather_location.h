#pragma once

#include <QtCore/QByteArray>

namespace Purple::PrivacyWeather {

enum class LocationAccess {
	Authorized,
	NotDetermined,
	Denied,
	Restricted,
	Disabled,
	Unavailable,
};

[[nodiscard]] inline QByteArray EffectiveSource(
		const QByteArray &saved,
		bool hasLegacyPlace) {
	return !saved.isEmpty() ? saved : QByteArray(hasLegacyPlace ? "place" : "current");
}

[[nodiscard]] inline bool CanRequestLocation(
		LocationAccess access,
		bool prompted) {
	return access == LocationAccess::Authorized
		|| (access == LocationAccess::NotDetermined && !prompted);
}

} // namespace Purple::PrivacyWeather

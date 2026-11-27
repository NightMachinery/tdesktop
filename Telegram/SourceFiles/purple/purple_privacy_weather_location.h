#pragma once

#include <QtCore/QByteArray>

#include <optional>

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
		const std::optional<QByteArray> &legacy) {
	if (!saved.isEmpty()) {
		return saved;
	}
	if (!legacy) {
		return "current";
	}
	return legacy->isEmpty() ? "off" : "place";
}

[[nodiscard]] inline bool CanRequestLocation(
		LocationAccess access,
		bool prompted) {
	return access == LocationAccess::Authorized
		|| (access == LocationAccess::NotDetermined && !prompted);
}

} // namespace Purple::PrivacyWeather

#pragma once

#include <QtNetwork/QNetworkProxy>

namespace Purple::PrivacyWeather {

[[nodiscard]] inline QNetworkProxy NetworkProxy(
		bool system,
		bool enabled,
		const QNetworkProxy &selected) {
	if (system) {
		return QNetworkProxy::DefaultProxy;
	}
	return (enabled && selected.type() != QNetworkProxy::DefaultProxy)
		? selected
		: QNetworkProxy(QNetworkProxy::NoProxy);
}

} // namespace Purple::PrivacyWeather

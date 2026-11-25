#pragma once

#include "purple/purple_settings.h"

#include <QtCore/QDateTime>
#include <QtCore/QTimeZone>
#include <set>

namespace Purple::PrivacyClocks {

struct Clock {
	QString label;
	QDateTime time;
	qint64 dayDifference = 0;
};

inline std::vector<Clock> At(
		const QDateTime &instant,
		const QTimeZone &system,
		const std::vector<WorldClock> &zones) {
	const auto local = instant.toTimeZone(system);
	auto offsets = std::set{ local.offsetFromUtc() };
	auto result = std::vector<Clock>();
	for (const auto &clock : zones) {
		const auto zone = QTimeZone(clock.zone.toUtf8());
		if (!zone.isValid()) {
			continue;
		}
		const auto time = instant.toTimeZone(zone);
		if (!offsets.insert(time.offsetFromUtc()).second) {
			continue;
		}
		auto label = clock.label.isEmpty() ? clock.zone.section(u'/', -1) : clock.label;
		result.push_back({
			label.replace(u'_', u' '),
			time,
			local.date().daysTo(time.date()),
		});
	}
	return result;
}

} // namespace Purple::PrivacyClocks

/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_seek_rules.h"

#include <algorithm>

namespace Purple::SeekRules {

int StepFrom(int seconds) {
	return (std::ranges::find(kSteps, seconds) != end(kSteps))
		? seconds
		: kStepDefault;
}

std::string StepSetting(int seconds) {
	return std::to_string(StepFrom(seconds)) + 's';
}

std::optional<std::int64_t> SeekTarget(
		std::int64_t positionMs,
		std::int64_t durationMs,
		int direction,
		int stepSeconds) {
	if (durationMs <= 0 || !direction) {
		return std::nullopt;
	}
	const auto position = std::clamp(positionMs, std::int64_t(0), durationMs);
	const auto last = std::max(durationMs - kEndGuardMs, std::int64_t(0));
	const auto delta = std::int64_t(StepFrom(stepSeconds)) * 1000;
	const auto target = (direction > 0)
		? std::max(std::min(position + delta, last), position)
		: std::max(position - delta, std::int64_t(0));
	if (target == position) {
		return std::nullopt;
	}
	return target;
}

} // namespace Purple::SeekRules

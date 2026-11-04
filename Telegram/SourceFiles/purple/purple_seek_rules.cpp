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

std::optional<std::int64_t> ToMs(std::int64_t value, int frequency) {
	if (frequency <= 0
		|| value < 0
		|| value / frequency > kLongestMs / 1000) {
		return std::nullopt;
	}
	const auto whole = value / frequency;
	return whole * 1000 + ((value % frequency) * 1000) / frequency;
}

SeekPlan PlanSeek(const SeekFacts &facts, int direction, int stepSeconds) {
	if (!facts.current) {
		const auto length = facts.endedDurationMs;
		if (!facts.ended || length <= 0 || length > kLongestMs) {
			return {};
		}
		const auto target = SeekTarget(length, length, direction, stepSeconds);
		if (!target) {
			return { .action = SeekAction::Consume };
		}
		return { .action = SeekAction::Restart, .restartAtMs = *target };
	}
	const auto duration = ToMs(facts.length, facts.frequency);
	const auto position = ToMs(facts.position, facts.frequency);
	if (facts.stopped || !duration || !*duration || !position) {
		return {};
	}
	const auto target = SeekTarget(*position, *duration, direction, stepSeconds);
	if (!target) {
		return { .action = SeekAction::Consume };
	}
	return {
		.action = facts.paused
			? SeekAction::SeekPaused
			: SeekAction::SeekPlaying,
		.progress = double(*target) / double(*duration),
	};
}

std::string SeekShortcutKeys(int direction, bool mac) {
	const auto arrow = (direction < 0) ? "left" : "right";
	return std::string(mac ? "ctrl+alt+" : "alt+shift+") + arrow;
}

} // namespace Purple::SeekRules

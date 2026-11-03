/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace Purple::SeekRules {

inline constexpr auto kSteps = std::array{ 5, 10, 15, 30 };
inline constexpr auto kStepDefault = 10;
inline constexpr auto kEndGuardMs = std::int64_t(1000);
inline constexpr auto kLongestMs = std::int64_t(0x7FFFFFFF);

[[nodiscard]] int StepFrom(int seconds);
[[nodiscard]] std::string StepSetting(int seconds);

[[nodiscard]] std::optional<std::int64_t> SeekTarget(
	std::int64_t positionMs,
	std::int64_t durationMs,
	int direction,
	int stepSeconds);

[[nodiscard]] std::optional<std::int64_t> ToMs(
	std::int64_t value,
	int frequency);

struct SeekFacts {
	bool current = false;
	bool stopped = false;
	bool paused = false;
	std::int64_t position = 0;
	std::int64_t length = 0;
	int frequency = 0;
	bool ended = false;
	std::int64_t endedDurationMs = 0;
};

enum class SeekAction {
	Decline,
	Consume,
	SeekPaused,
	SeekPlaying,
	Restart,
};

struct SeekPlan {
	SeekAction action = SeekAction::Decline;
	double progress = 0.;
	std::int64_t restartAtMs = 0;
};

[[nodiscard]] SeekPlan PlanSeek(
	const SeekFacts &facts,
	int direction,
	int stepSeconds);

[[nodiscard]] std::string SeekShortcutKeys(int direction, bool mac);

} // namespace Purple::SeekRules

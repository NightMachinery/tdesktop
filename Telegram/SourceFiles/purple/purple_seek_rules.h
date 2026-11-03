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

[[nodiscard]] int StepFrom(int seconds);
[[nodiscard]] std::string StepSetting(int seconds);

[[nodiscard]] std::optional<std::int64_t> SeekTarget(
	std::int64_t positionMs,
	std::int64_t durationMs,
	int direction,
	int stepSeconds);

} // namespace Purple::SeekRules

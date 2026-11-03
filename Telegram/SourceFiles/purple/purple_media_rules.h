/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <array>
#include <optional>
#include <string_view>

namespace Purple::MediaRules {

struct RestartFacts {
	bool leftButton = false;
	bool selecting = false;
	bool documentOpen = false;
	bool voiceOrRound = false;
	bool viewOnce = false;
	bool currentTrack = false;
	bool seekable = false;
};

[[nodiscard]] bool RestartsOnDoubleClick(const RestartFacts &facts);

inline constexpr auto kVideoMessageScales = std::array{ 100, 150, 200, 250 };
inline constexpr auto kVideoMessageScaleDefault = 200;
inline constexpr auto kVideoMessageScaleActual = 0;
inline constexpr auto kVideoMessageReferenceSource = 400;
inline constexpr auto kVideoMessageActualText = std::string_view("actual");

[[nodiscard]] int VideoMessageScaleFrom(std::optional<int> stored);
[[nodiscard]] int VideoMessageScaleFromText(std::string_view stored);
[[nodiscard]] int VideoMessageActualSide(int sourceSide, double ratio);
[[nodiscard]] int VideoMessageActualPercent(
	int sourceSide,
	double ratio,
	int stockSide);
[[nodiscard]] int VideoMessageSide(int percent, int stockSide);
[[nodiscard]] int VideoMessageThumbSide(int side, int widthMax, int optimalBound);
[[nodiscard]] int VideoMessageBubbleLimit(int maxWidth, int stockLimit);

} // namespace Purple::MediaRules

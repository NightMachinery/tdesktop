/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_media_rules.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace Purple::MediaRules {

Restart RestartOnDoubleClick(const RestartFacts &facts) {
	const auto restarts = facts.leftButton
		&& !facts.selecting
		&& facts.documentOpen
		&& facts.voiceOrRound
		&& !facts.viewOnce
		&& facts.currentTrack;
	return !restarts
		? Restart::None
		: facts.seekable
		? Restart::Seek
		: Restart::Replay;
}

int VideoMessageScaleFrom(std::optional<int> stored) {
	const auto offered = stored
		&& ((*stored == kVideoMessageScaleActual)
			|| std::ranges::find(kVideoMessageScales, *stored)
				!= end(kVideoMessageScales));
	return offered ? *stored : kVideoMessageScaleDefault;
}

int VideoMessageScaleFromText(std::string_view stored) {
	if (stored == kVideoMessageActualText) {
		return kVideoMessageScaleActual;
	}
	auto value = 0;
	const auto end = stored.data() + stored.size();
	const auto [last, error] = std::from_chars(stored.data(), end, value);
	return (error == std::errc() && last == end && value > 0)
		? VideoMessageScaleFrom(value)
		: kVideoMessageScaleDefault;
}

int VideoMessageActualSide(int sourceSide, double ratio) {
	const auto source = (sourceSide > 0)
		? sourceSide
		: kVideoMessageReferenceSource;
	return std::max(int(std::lround(source / std::max(ratio, 1.))), 1);
}

int VideoMessageActualPercent(int sourceSide, double ratio, int stockSide) {
	const auto side = VideoMessageActualSide(sourceSide, ratio);
	return int(std::lround(side * 100. / std::max(stockSide, 1)));
}

int VideoMessageSide(int percent, int stockSide) {
	return std::max(stockSide * percent / 100, 1);
}

int VideoMessageThumbSide(int side, int widthMax, int optimalBound) {
	// WHY: Gif::countOptimalSize passes st::msgMaxWidth as a fixed bound, not
	// the room there is; capping by it there would keep every size at 430px.
	// Every other caller passes the width the round may really take.
	return (widthMax == optimalBound)
		? side
		: std::clamp(widthMax, 1, side);
}

int VideoMessageBubbleLimit(int maxWidth, int stockLimit) {
	return (maxWidth > stockLimit) ? maxWidth : 0;
}

int FloatPlayerSide(int stockSize, int margins, int side, int windowShortSide) {
	const auto wanted = margins + side;
	const auto room = (windowShortSide > 0) ? (windowShortSide / 2) : wanted;
	return std::max(std::min(wanted, room), stockSize);
}

} // namespace Purple::MediaRules

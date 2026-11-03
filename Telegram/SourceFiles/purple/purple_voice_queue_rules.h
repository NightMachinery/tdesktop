/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_playback.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

namespace Purple::VoiceQueueRules {

enum class Candidate {
	Missing,
	Incoming,
	Outgoing,
};

[[nodiscard]] std::optional<int> ConfinedIndex(
	VoiceQueueAnchor anchor,
	int from,
	const std::function<Candidate(int)> &at);

using PlayKey = std::pair<std::uint64_t, std::int64_t>;

class PlayTracker final {
public:
	void automaticNext(PlayKey key);
	[[nodiscard]] bool manual(PlayKey played);

private:
	std::optional<PlayKey> _automatic;

};

} // namespace Purple::VoiceQueueRules

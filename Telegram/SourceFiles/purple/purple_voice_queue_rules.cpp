/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_voice_queue_rules.h"

namespace Purple::VoiceQueueRules {

std::optional<int> ConfinedIndex(
		VoiceQueueAnchor anchor,
		int from,
		const std::function<Candidate(int)> &at) {
	if (!anchor.confined) {
		return std::nullopt;
	}
	for (auto index = from;; ++index) {
		const auto candidate = at(index);
		if (candidate == Candidate::Missing) {
			return -1;
		} else if (QueueKeeps(anchor, candidate == Candidate::Outgoing)) {
			return index;
		}
	}
}

void PlayTracker::automaticNext(PlayKey key) {
	_automatic = key;
}

bool PlayTracker::manual(PlayKey played) {
	const auto automatic = std::exchange(_automatic, std::nullopt);
	return !automatic || (*automatic != played);
}

} // namespace Purple::VoiceQueueRules

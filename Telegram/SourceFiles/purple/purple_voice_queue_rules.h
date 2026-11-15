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

struct ChatFacts {
	bool self = false;
	bool user = false;
	bool replies = false;
	bool verifyCodes = false;
};

[[nodiscard]] PlaybackChat PlaybackChatFor(ChatFacts facts);

[[nodiscard]] std::optional<int> ConfinedIndex(
	VoiceQueueAnchor anchor,
	int from,
	const std::function<Candidate(int)> &at);

using PlayKey = std::pair<std::uint64_t, std::int64_t>;

class PlayTracker final {
public:
	void automaticNext(PlayKey key);
	void expire(PlayKey key);
	[[nodiscard]] bool manual(PlayKey played);

private:
	std::optional<PlayKey> _automatic;

};

struct ContinuationCandidate {
	Candidate direction = Candidate::Missing;
	bool unheard = false;
};

class Continuation final {
public:
	void begin(
		const Playback &settings,
		PlaybackChat chat,
		bool outgoing);
	void clear();
	void close();
	[[nodiscard]] bool permitsAdvance() const;
	[[nodiscard]] bool resumesMusic() const;
	[[nodiscard]] std::optional<int> next(
		int from,
		const std::function<ContinuationCandidate(int)> &at);

private:
	struct State {
		VoiceQueueAnchor ownership;
		VoiceContinuation mode = VoiceContinuation::Unheard;
		bool resumeMusic = true;
		bool closed = false;
	};
	std::optional<State> _state;

};

} // namespace Purple::VoiceQueueRules

/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_voice_queue_rules.h"

#include <limits>

namespace Purple::VoiceQueueRules {

PlaybackChat PlaybackChatFor(ChatFacts facts) {
	return facts.self
		? PlaybackChat::Self
		: (facts.user && !facts.replies && !facts.verifyCodes)
		? PlaybackChat::OneToOne
		: PlaybackChat::Other;
}

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

void PlayTracker::expire(PlayKey key) {
	if (_automatic == key) {
		_automatic = std::nullopt;
	}
}

bool PlayTracker::manual(PlayKey played) {
	const auto automatic = std::exchange(_automatic, std::nullopt);
	return !automatic || (*automatic != played);
}

void Continuation::begin(
		const Playback &settings,
		PlaybackChat chat,
		bool outgoing) {
	_state = State{
		.ownership = AnchorForManualPlay(settings, chat, outgoing),
		.mode = settings.afterVoice,
		.resumeMusic = settings.resumeMusicAfterVoice,
	};
}

void Continuation::clear() {
	_state.reset();
}

void Continuation::close() {
	if (_state) _state->closed = true;
}

bool Continuation::permitsAdvance() const {
	return !_state || (!_state->closed
		&& _state->mode != VoiceContinuation::ResumeMusic);
}

bool Continuation::resumesMusic() const {
	return !_state || _state->resumeMusic;
}

std::optional<int> Continuation::next(
		int from,
		const std::function<ContinuationCandidate(int)> &at) {
	if (!_state) return std::nullopt;
	if (!permitsAdvance()) return -1;
	for (auto index = from;; ++index) {
		const auto candidate = at(index);
		if (candidate.direction == Candidate::Missing) {
			_state->closed = true;
			return -1;
		}
		const auto outgoing = candidate.direction == Candidate::Outgoing;
		const auto decision = ContinuationDecision(
			_state->mode,
			outgoing,
			candidate.unheard,
			QueueKeeps(_state->ownership, outgoing));
		if (decision == VoiceQueueDecision::Play) return index;
		if (decision == VoiceQueueDecision::Stop
			|| index == std::numeric_limits<int>::max()) {
			_state->closed = true;
			return -1;
		}
	}
}

} // namespace Purple::VoiceQueueRules

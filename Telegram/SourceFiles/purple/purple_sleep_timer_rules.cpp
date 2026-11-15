#include "purple/purple_sleep_timer_rules.h"

#include <algorithm>
#include <limits>

namespace Purple::SleepTimerRules {
namespace {

Time Deadline(Time now, Time duration) {
	return now > std::numeric_limits<Time>::max() - duration
		? std::numeric_limits<Time>::max() : now + duration;
}

void Disarm(State &state) {
	state.mode = Mode::Off;
	state.deadline = 0;
	state.bound.reset();
	state.stream = 0;
	state.owner = 0;
	++state.generation;
}

} // namespace

void BeginSeek(SeekIntent &intent, bool userPaused) {
	intent = { .pending = true, .keepPaused = userPaused, .temporaryPause = true };
}

void BeginAudioSeek(SeekIntent &intent, bool observedPaused, bool sameItem) {
	if (!sameItem || !intent.pending) BeginSeek(intent, observedPaused);
	intent.temporaryPause = true;
}

void PauseSeek(SeekIntent &intent) {
	if (!intent.pending) return;
	if (intent.temporaryPause) intent.temporaryPause = false;
	else intent.keepPaused = true;
}

void ExpireSeek(SeekIntent &intent) {
	if (intent.pending) {
		intent.keepPaused = true;
		intent.temporaryPause = false;
	}
}

void SetSeekPlaybackIntent(SeekIntent &intent, bool keepPaused) {
	if (intent.pending) {
		intent.keepPaused = keepPaused;
		intent.temporaryPause = false;
	}
}

bool FinishSeek(SeekIntent &intent, bool held, bool actualPaused) {
	const auto paused = intent.pending && held ? intent.keepPaused : actualPaused;
	intent = {};
	return paused;
}

void BeginVideoSeek(SeekIntent &intent, bool paused) {
	if (!intent.pending) BeginSeek(intent, paused);
	if (!paused) intent.temporaryPause = true;
}

void SetVideoSeekPlaybackIntent(
		SeekIntent &intent, bool pause, bool &restartPaused, bool &pausedBySeek) {
	restartPaused = pause;
	SetSeekPlaybackIntent(intent, pause);
	pausedBySeek = intent.pending && !pause;
}

void ToggleVideoSeekPlaybackIntent(
		SeekIntent &intent, bool paused, bool active, bool finished,
		bool &restartPaused, bool &pausedBySeek) {
	if (!paused && intent.pending && intent.temporaryPause) {
		PauseSeek(intent);
		return;
	}
	SetVideoSeekPlaybackIntent(intent, active && !finished && !paused,
		restartPaused, pausedBySeek);
}

void PreserveVideoSeek(
		SeekIntent &intent, bool active, bool &restartPaused, bool &pausedBySeek) {
	pausedBySeek = false;
	if (active) restartPaused = true;
	ExpireSeek(intent);
}

void PauseForExpiry(const PausePorts &ports) {
	ports.clearAudioResumes();
	ports.clearVideoResumes();
	ports.pauseVoice();
	ports.pauseSong();
	ports.pauseVideos();
}

bool ValidDuration(Time duration) {
	return duration >= kMinute && duration <= kMaximum
		&& duration % kMinute == 0;
}

Time Remaining(const State &state, Time now) {
	return state.mode == Mode::Timed && state.deadline > now
		? (now < 0 ? kMaximum : std::min(kMaximum, state.deadline - now)) : 0;
}

Time CustomPrefill(const State &state, Time now, Time lastCustom) {
	const auto remaining = Remaining(state, now);
	return state.mode == Mode::Timed
		? std::clamp(((remaining + kMinute - 1) / kMinute) * kMinute, kMinute, kMaximum)
		: ValidDuration(lastCustom) ? lastCustom : 45 * kMinute;
}

std::optional<Media> ChooseEnd(const std::vector<Media> &media) {
	for (const auto kind : { Kind::Voice, Kind::Video, Kind::Song }) {
		auto chosen = std::optional<Media>();
		for (const auto &entry : media) {
			if (entry.loaded && entry.eligible && entry.item.kind == kind
				&& (!chosen || entry.owner < chosen->owner)) {
				chosen = entry;
			}
		}
		if (chosen) return chosen;
	}
	return std::nullopt;
}

bool StartTimed(State &state, Time now, Time duration, bool loaded) {
	if (!loaded || !ValidDuration(duration) || now < 0) return false;
	Cancel(state);
	state.mode = Mode::Timed;
	state.deadline = Deadline(now, duration);
	return true;
}

bool StartEnd(State &state, const std::optional<Media> &media) {
	if (!media || !media->loaded || !media->eligible) return false;
	Cancel(state);
	state.mode = Mode::EndOfItem;
	state.bound = media->item;
	state.stream = media->stream;
	state.owner = media->owner;
	return true;
}

bool AddTime(State &state, Time now, Time duration) {
	if (state.mode != Mode::Timed || duration <= 0 || now < 0) return false;
	state.deadline = Deadline(now, std::min(kMaximum, Remaining(state, now) + std::min(duration, kMaximum)));
	++state.generation;
	return true;
}

bool Expire(State &state, Time now) {
	if (state.mode != Mode::Timed || now < state.deadline) return false;
	Disarm(state);
	return true;
}

bool Finish(State &state, const Item &item, std::uint64_t stream) {
	if (state.mode != Mode::EndOfItem || state.bound != item
		|| (item.kind == Kind::Video && state.stream != stream)) return false;
	Disarm(state);
	if (item.kind == Kind::Song) state.pendingAdvance = item;
	return true;
}

bool TakeAdvance(State &state, const Item &item) {
	const auto take = state.pendingAdvance == item;
	if (item.kind == Kind::Song) state.pendingAdvance.reset();
	return take;
}

void DropAdvance(State &state, Kind kind) {
	if (kind == Kind::Song) state.pendingAdvance.reset();
}

void Cancel(State &state) {
	Disarm(state);
}

Loss Reconcile(State &state, const std::vector<Media> &media) {
	if (state.mode == Mode::Off) return Loss::None;
	const auto loaded = std::any_of(media.begin(), media.end(), [](const Media &entry) {
		return entry.loaded;
	});
	if (!loaded) {
		Cancel(state);
		return Loss::Silent;
	}
	if (state.mode == Mode::Timed) return Loss::None;
	const auto bound = *state.bound;
	const auto same = std::find_if(media.begin(), media.end(), [&](const Media &entry) {
		return entry.loaded && entry.eligible && entry.item == bound
			&& (bound.kind != Kind::Video || entry.stream == state.stream);
	});
	if (same != media.end()) {
		state.owner = same->owner;
		return Loss::None;
	}
	const auto replacement = std::find_if(media.begin(), media.end(), [&](const Media &entry) {
		return entry.loaded && entry.eligible && entry.item.kind == bound.kind
			&& entry.owner == state.owner;
	});
	if (replacement != media.end()) {
		state.bound = replacement->item;
		state.stream = replacement->stream;
		++state.generation;
		return Loss::None;
	}
	Cancel(state);
	return Loss::Notice;
}

} // namespace Purple::SleepTimerRules

#include "purple/purple_sleep_timer_rules.h"

#include <iostream>
#include <limits>
#include <string>

namespace {

using namespace Purple::SleepTimerRules;
int Checks = 0;
int Failures = 0;

void Expect(bool value, const char *name) {
	++Checks;
	if (!value) {
		++Failures;
		std::cerr << name << '\n';
	}
}

Media Song(std::uint64_t document = 10) {
	return { .item = { .kind = Kind::Song, .session = 1, .document = document, .peer = 2, .message = 3 },
		.owner = 1, .loaded = true, .eligible = true, .playing = true };
}

Media Video(std::uint64_t owner = 3, std::uint64_t stream = 50) {
	return { .item = { .kind = Kind::Video, .session = 1, .document = 20, .peer = 2, .message = 4 },
		.owner = owner, .stream = stream, .loaded = true, .eligible = true, .playing = true };
}

void RepeatedAudioSeekCases() {
	auto seek = SeekIntent();
	State timer;
	BeginAudioSeek(seek, false, false);
	PauseSeek(seek);
	BeginAudioSeek(seek, true, true);
	PauseSeek(seek);
	Expect(timer.mode == Mode::Off && !FinishSeek(seek, true, true),
		"ordinary playing drag with two start/pause events resumes with timer Off");

	BeginAudioSeek(seek, false, false);
	PauseSeek(seek);
	ExpireSeek(seek);
	BeginAudioSeek(seek, true, true);
	PauseSeek(seek);
	Expect(FinishSeek(seek, true, true), "held expiry survives repeated start and its own pause");

	BeginAudioSeek(seek, false, false);
	PauseSeek(seek);
	ExpireSeek(seek);
	SetSeekPlaybackIntent(seek, false);
	BeginAudioSeek(seek, false, true);
	PauseSeek(seek);
	BeginAudioSeek(seek, true, true);
	PauseSeek(seek);
	Expect(!FinishSeek(seek, true, true),
		"held explicit Play survives two more move/start/own-pause events");

	BeginAudioSeek(seek, false, false);
	PauseSeek(seek);
	SetSeekPlaybackIntent(seek, true);
	BeginAudioSeek(seek, true, true);
	PauseSeek(seek);
	BeginAudioSeek(seek, true, true);
	PauseSeek(seek);
	Expect(FinishSeek(seek, true, true), "deliberate Pause wins across repeated audio starts");

	BeginAudioSeek(seek, true, false);
	PauseSeek(seek);
	BeginAudioSeek(seek, true, true);
	PauseSeek(seek);
	Expect(FinishSeek(seek, true, true), "already user-paused drag stays paused across repeated starts");

	BeginAudioSeek(seek, false, false);
	PauseSeek(seek);
	SetSeekPlaybackIntent(seek, false);
	BeginAudioSeek(seek, true, true);
	Expect(seek.temporaryPause && !seek.keepPaused,
		"same-item port fact preserves intent and rearms own pause after transient id rotation");
	PauseSeek(seek);
	Expect(!FinishSeek(seek, true, true), "same-item id refresh does not recapture observed temporary pause");

	BeginAudioSeek(seek, false, false);
	PauseSeek(seek);
	ExpireSeek(seek);
	BeginAudioSeek(seek, false, false);
	PauseSeek(seek);
	Expect(!FinishSeek(seek, true, true), "actual item replacement captures its own playing intent");
	BeginAudioSeek(seek, true, true);
	PauseSeek(seek);
	Expect(FinishSeek(seek, true, true), "new drag after finish captures actual user-paused state");
}

void Exercise() {
	RepeatedAudioSeekCases();
	{
		auto seek = SeekIntent();
		BeginSeek(seek, false);
		PauseSeek(seek);
		Expect(!seek.keepPaused, "stock temporary seek pause preserves playing intent");
		ExpireSeek(seek);
		Expect(FinishSeek(seek, true, true), "held seek release preserves expiry pause");
		Expect(!seek.pending, "finish consumes seek intent instead of latching forever");
		Expect(!FinishSeek(seek, false, false), "later independent playing seek may play");
		Expect(FinishSeek(seek, false, true), "later independent user-paused seek stays paused");
		BeginSeek(seek, false);
		PauseSeek(seek);
		ExpireSeek(seek);
		SetSeekPlaybackIntent(seek, false);
		Expect(!FinishSeek(seek, true, false), "explicit Play supersedes held expiry pause");
		BeginSeek(seek, false);
		PauseSeek(seek);
		SetSeekPlaybackIntent(seek, true);
		Expect(FinishSeek(seek, true, true), "explicit Pause supersedes earlier seek resume");
		BeginSeek(seek, true);
		PauseSeek(seek);
		ExpireSeek(seek);
		Expect(FinishSeek(seek, true, true), "expiry never toggles an already user-paused seek");
		BeginSeek(seek, false);
		PauseSeek(seek);
		ExpireSeek(seek);
		seek = {};
		Expect(!FinishSeek(seek, false, false), "retiring item or cancelling seek drops old expiry intent");
	}

	{
		auto seek = SeekIntent();
		auto restartPaused = false;
		auto pausedBySeek = true;
		BeginVideoSeek(seek, false);
		ToggleVideoSeekPlaybackIntent(seek, false, true, false, restartPaused, pausedBySeek);
		Expect(!seek.keepPaused && pausedBySeek, "viewer progress pause preserves normal drag resume");
		PreserveVideoSeek(seek, true, restartPaused, pausedBySeek);
		Expect(restartPaused && !pausedBySeek && FinishSeek(seek, true, true),
			"already-paused viewer/PiP consumes held resume before expiry guard");
		ToggleVideoSeekPlaybackIntent(seek, true, true, false, restartPaused, pausedBySeek);
		Expect(!restartPaused, "manual Play clears stale viewer restart pause for J/L, numeric and chapter seek");
		ToggleVideoSeekPlaybackIntent(seek, false, true, false, restartPaused, pausedBySeek);
		Expect(restartPaused, "manual Pause retains restart pause for keyboard and chapter seeks");
		BeginVideoSeek(seek, false);
		PauseSeek(seek);
		PreserveVideoSeek(seek, true, restartPaused, pausedBySeek);
		ToggleVideoSeekPlaybackIntent(seek, true, true, false, restartPaused, pausedBySeek);
		Expect(!restartPaused && pausedBySeek, "explicit Play while held supersedes expiry");
		BeginVideoSeek(seek, false);
		ToggleVideoSeekPlaybackIntent(seek, false, true, false, restartPaused, pausedBySeek);
		Expect(!FinishSeek(seek, true, true), "further held progress does not discard explicit Play intent");
	}

	State state;
	Expect(state.mode == Mode::Off, "new instance never restores an active timer");
	Expect(!StartTimed(state, 100, kMinute, false), "no loaded media refuses timed shortcut");
	Expect(!StartEnd(state, std::nullopt), "no eligible end item has no timed fallback");
	for (const auto duration : { Time(0), kMinute - 1, kMaximum + kMinute, Time(-1) }) {
		Expect(!ValidDuration(duration), "custom bounds refuse zero, fraction, negative and excess");
	}
	Expect(ValidDuration(kMinute) && ValidDuration(kMaximum), "custom endpoints accepted");
	Expect(StartTimed(state, 100, 15 * kMinute, true), "timed mode starts while loaded");
	Expect(!Expire(state, 100 + 15 * kMinute - 1), "deadline does not expire early");
	Expect(Expire(state, 100 + 15 * kMinute) && !Expire(state, 100 + 15 * kMinute + 1),
		"deadline expires exactly once");
	Expect(StartTimed(state, 100, kMinute, true), "timer can be started after expiry");
	Expect(!Expire(state, 50) && state.deadline == 100 + kMinute,
		"earlier clock observation does not move the deadline");
	Expect(Expire(state, 20 * kMinute), "adjusted wake time past deadline expires");
	Expect(StartTimed(state, 100, kMaximum, true), "maximum duration starts");
	Expect(AddTime(state, 200, std::numeric_limits<Time>::max()) && Remaining(state, 200) == kMaximum,
		"large add caps remaining time without overflow");
	Expect(StartTimed(state, std::numeric_limits<Time>::max() - 10, kMinute, true),
		"large elapsed time saturates deadline safely");
	Expect(state.deadline == std::numeric_limits<Time>::max(), "saturated deadline is representable");
	Expect(StartTimed(state, 0, 2 * kMinute, true), "prefill fixture starts");
	Expect(CustomPrefill(state, 2 * kMinute - 1, 45 * kMinute) == kMinute,
		"remaining custom prefill rounds up with one-minute minimum");
	Cancel(state);
	Expect(CustomPrefill(state, 0, 0) == 45 * kMinute, "first custom fallback is 45 minutes");
	Expect(CustomPrefill(state, 0, 7 * kMinute) == 7 * kMinute, "last valid custom retained");

	auto song = Song();
	auto voice = Song(30);
	voice.item.kind = Kind::Voice;
	voice.owner = 2;
	voice.playing = false;
	Expect(ChooseEnd({ song, Video(), voice })->item.kind == Kind::Voice,
		"paused eligible voice takes priority, then video, then song");
	Expect(ChooseEnd({ song, Video() })->item.kind == Kind::Video, "video precedes song");
	voice.eligible = false;
	Expect(ChooseEnd({ song, voice })->item.kind == Kind::Song, "finished voice is not eligible");
	Expect(StartEnd(state, song), "unknown duration needs only an eligible end event");
	auto next = Song(11);
	Expect(Reconcile(state, { next }) == Loss::None && state.bound == next.item,
		"manual change on bound player rebinds");
	Expect(!Finish(state, song.item) && Finish(state, next.item),
		"only rebound item consumes end before repeat or queue movement");
	Expect(state.mode == Mode::Off && state.pendingAdvance == next.item,
		"song end disarms timer and retains pending manual advance");
	Expect(!Finish(state, next.item), "duplicate end cannot repeat expiry");
	Expect(TakeAdvance(state, next.item) && !TakeAdvance(state, next.item),
		"manual song advance is consumed once");
	Expect(StartEnd(state, song) && Finish(state, song.item), "pending seek fixture ends");
	DropAdvance(state, Kind::Song);
	Expect(!TakeAdvance(state, song.item), "seek, explicit play or stop drops pending advance");
	voice = { .item = { .kind = Kind::Voice, .session = 1, .document = 31 },
		.owner = 2, .loaded = true, .eligible = true };
	Expect(StartEnd(state, voice) && Finish(state, voice.item) && !state.pendingAdvance,
		"voice end has no song advance behavior");

	auto viewer = Video();
	Expect(StartEnd(state, viewer), "viewer binding starts");
	auto pip = viewer;
	pip.owner = 4;
	Expect(Reconcile(state, { pip }) == Loss::None && state.owner == 4,
		"same document, message and shared stream hand off to PiP");
	Expect(!Finish(state, pip.item, pip.stream + 1), "other stream cannot consume bound end");
	Expect(Finish(state, pip.item, pip.stream), "bound shared stream end expires");
	Expect(StartEnd(state, viewer), "viewer loss fixture starts");
	auto other = Video(4, 80);
	other.item.document = 21;
	Expect(Reconcile(state, { other, song }) == Loss::Notice && state.mode == Mode::Off,
		"closed owner never rebinds to an unrelated remaining player");
	Expect(StartEnd(state, viewer), "silent replacement fixture starts");
	other = Video();
	other.item.document = 22;
	other.eligible = false;
	Expect(Reconcile(state, { other }) == Loss::Notice, "silent or looping replacement loses end binding");
	Expect(StartEnd(state, viewer), "same-item eligibility loss fixture starts");
	viewer.eligible = false;
	Expect(Reconcile(state, { viewer }) == Loss::Notice,
		"same item becoming silent or looping cancels rather than waiting forever");
	Expect(StartTimed(state, 0, kMinute, true), "loaded-loss timed fixture starts");
	Expect(Reconcile(state, {}) == Loss::Silent && state.mode == Mode::Off,
		"nothing loaded cancels silently rather than affecting later playback");

	bool audioResume = true;
	bool videoResume = true;
	bool buffering = true;
	bool pausedByUser = false;
	auto order = std::string();
	auto pausing = 0;
	const auto ports = PausePorts{
		.clearAudioResumes = [&] { audioResume = false; order += 'a'; },
		.clearVideoResumes = [&] { videoResume = false; order += 'v'; },
		.pauseVoice = [&] { Expect(!audioResume && !videoResume, "resume flags cleared before voice pause"); order += 'V'; },
		.pauseSong = [&] { Expect(!audioResume && !videoResume, "resume flags cleared before song pause"); order += 'S'; },
		.pauseVideos = [&] { pausedByUser = true; ++pausing; order += 'P'; },
	};
	Expect(StartTimed(state, 0, kMinute, true), "orchestration timer starts");
	if (Expire(state, kMinute)) PauseForExpiry(ports);
	if (Expire(state, 2 * kMinute)) PauseForExpiry(ports);
	Expect(order == "avVSP" && pausing == 1, "expiry orchestration clears all resumes before pause, once");
	buffering = false;
	Expect(!buffering && pausedByUser && !audioResume && !videoResume,
		"buffer completion or call/voice completion cannot start paused media");
	pausedByUser = false;
	Expect(state.mode == Mode::Off && !pausedByUser, "manual resume does not rearm timer");
	PauseForExpiry(ports);
	Expect(pausedByUser && !audioResume && !videoResume, "pause ports remain idempotent and never toggle play");
}

} // namespace

int main() {
	Exercise();
	std::cout << "SLEEP_TIMER_CHECKS=" << Checks << " FAILURES=" << Failures << '\n';
	return Failures ? 1 : 0;
}

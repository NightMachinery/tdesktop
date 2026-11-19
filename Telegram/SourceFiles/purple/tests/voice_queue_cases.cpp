#include "purple/purple_voice_queue_rules.h"

#include <iostream>
#include <vector>

int OriginalVoiceQueueCases() {
	auto checks = 0;
	auto failures = 0;
	const auto expect = [&](bool value, const char *name) {
		++checks;
		if (!value) { ++failures; std::cerr << name << '\n'; }
	};
	using namespace Purple;
	using namespace Purple::VoiceQueueRules;
	const auto in = Candidate::Incoming;
	const auto out = Candidate::Outgoing;
	const auto table = [](std::vector<Candidate> items) {
		return [items = std::move(items)](int index) {
			return (index >= 0 && index < int(items.size()))
				? items[index]
				: Candidate::Missing;
		};
	};
	auto on = Playback();
	auto off = Playback();
	off.privateChatQueue = false;
	const auto confined = AnchorForManualPlay(on, PlaybackChat::OneToOne, false);
	expect(confined.confined, "their message in a private chat confines");

	expect(ConfinedIndex(confined, 1, table({ in, in, in })) == 1, "their next message plays");
	expect(ConfinedIndex(confined, 1, table({ in, out, in })) == 2, "your message is skipped");
	expect(ConfinedIndex(confined, 1, table({ in, out, out, out, in })) == 4, "a run of yours is skipped");
	expect(ConfinedIndex(confined, 1, table({ in, out, out })) == -1, "the slice edge ends the queue");
	expect(ConfinedIndex(confined, 3, table({ in, in })) == -1, "nothing left ends the queue");

	const auto unconfined = [&](VoiceQueueAnchor anchor, const char *name) {
		expect(!ConfinedIndex(anchor, 1, table({ in, out, in })), name);
	};
	unconfined(AnchorForManualPlay(on, PlaybackChat::OneToOne, true), "your own message does not confine");
	unconfined(AnchorForManualPlay(on, PlaybackChat::Self, false), "Saved Messages do not confine");
	unconfined(AnchorForManualPlay(on, PlaybackChat::Other, false), "groups and channels do not confine");
	unconfined(AnchorForManualPlay(off, PlaybackChat::OneToOne, false), "the setting off does not confine");
	unconfined(VoiceQueueAnchor(), "no anchor does not confine");

	auto visits = 0;
	const auto counted = [&](int index) {
		++visits;
		return (index < 40) ? out : Candidate::Missing;
	};
	expect(ConfinedIndex(confined, 0, counted) == -1 && visits == 41, "the walk stops at the first missing index");

	const auto a = PlayKey{ 1, 10 };
	const auto b = PlayKey{ 1, 11 };
	const auto c = PlayKey{ 1, 12 };
	auto plays = PlayTracker();
	expect(plays.manual(a), "a track change with nothing announced is manual");
	plays.automaticNext(b);
	expect(!plays.manual(b), "the announced next track is automatic");
	expect(plays.manual(c), "a later change without an announcement is manual");
	plays.automaticNext(b);
	expect(plays.manual(c), "next or previous after an announcement for another track is manual");
	expect(plays.manual(b), "an announcement is used up by the first track change");
	plays.automaticNext(PlayKey{ 2, 10 });
	expect(plays.manual(a), "the same message id in another chat is manual");

	plays.automaticNext(b);
	plays.expire(b);
	expect(plays.manual(b), "an announcement no track change followed has expired");
	plays.automaticNext(b);
	plays.expire(c);
	expect(!plays.manual(b), "expiring another announcement keeps this one");

	expect(PlaybackChatFor({ .user = true }) == PlaybackChat::OneToOne, "a user or bot chat is one-to-one");
	expect(PlaybackChatFor({ .self = true, .user = true }) == PlaybackChat::Self, "Saved Messages are Self");
	expect(PlaybackChatFor({ .user = true, .replies = true }) == PlaybackChat::Other, "Replies is not one-to-one");
	expect(PlaybackChatFor({ .user = true, .verifyCodes = true }) == PlaybackChat::Other, "Verification Codes is not one-to-one");
	expect(PlaybackChatFor({}) == PlaybackChat::Other, "a group or channel is Other");

	std::cout << (checks - failures) << '/' << checks << " voice queue checks passed\n";
	return failures ? 1 : 0;
}

#include <limits>
int ContinuationPolicyCases() {
	using namespace Purple;
	using namespace Purple::VoiceQueueRules;
	auto checks = 0;
	auto failures = 0;
	const auto expect = [&](bool value, const char *name) {
		++checks;
		if (!value) {
			++failures;
			std::cerr << name << '\n';
		}
	};
	const auto table = [](std::vector<ContinuationCandidate> values) {
		return [values = std::move(values)](int index) {
			return index >= 0 && index < int(values.size()) ? values[index]
															: ContinuationCandidate();
		};
	};
	const auto incoming = ContinuationCandidate{Candidate::Incoming, true};
	const auto heard = ContinuationCandidate{Candidate::Incoming, false};
	const auto own = ContinuationCandidate{Candidate::Outgoing, false};
	const auto ownUnheard = ContinuationCandidate{Candidate::Outgoing, true};
	auto queue = Continuation();
	expect(!queue.next(0, table({incoming})),
		   "unbound continuation delegates to native queue");
	expect(queue.permitsAdvance() && queue.resumesMusic(),
		   "unbound continuation preserves native defaults");
	auto closed = Continuation();
	auto noResume = Playback();
	noResume.resumeMusicAfterVoice = false;
	closed.begin(noResume, PlaybackChat::Other, false);
	closed.close();
	expect(!closed.permitsAdvance() && !closed.resumesMusic(),
		   "Stop closes advance and retains resume-off");
	noResume.resumeMusicAfterVoice = true;
	closed.begin(noResume, PlaybackChat::Other, false);
	closed.close();
	expect(!closed.permitsAdvance() && closed.resumesMusic(),
		   "Stop retains captured resume-on");
	closed.begin(noResume, PlaybackChat::Other, false);
	expect(closed.permitsAdvance(), "new manual begin reopens a stopped queue");
	auto settings = Playback();
	for (const auto mode : {VoiceContinuation::ResumeMusic, VoiceContinuation::Unheard,
							VoiceContinuation::AllFollowing}) {
		for (const auto chat :
			 {PlaybackChat::OneToOne, PlaybackChat::Self, PlaybackChat::Other}) {
			for (const auto privateQueue : {false, true}) {
				for (const auto startOutgoing : {false, true}) {
					settings.afterVoice = mode;
					settings.privateChatQueue = privateQueue;
					queue.begin(settings, chat, startOutgoing);
					const auto confined =
						chat == PlaybackChat::OneToOne && privateQueue && !startOutgoing;
					const auto expected = mode == VoiceContinuation::ResumeMusic ? -1
										  : mode == VoiceContinuation::Unheard || confined
											  ? 1
											  : 0;
					expect(queue.next(0, table({own, incoming})) == expected,
						   "mode and independent ownership snapshot select the correct "
						   "following item");
				}
			}
		}
	}
	settings.afterVoice = VoiceContinuation::Unheard;
	settings.resumeMusicAfterVoice = false;
	queue.begin(settings, PlaybackChat::Other, false);
	settings.afterVoice = VoiceContinuation::AllFollowing;
	settings.resumeMusicAfterVoice = true;
	expect(!queue.resumesMusic(),
		   "resume preference remains captured while settings change");
	expect(queue.next(0, table({ownUnheard, incoming})) == 1,
		   "mode remains captured while settings change and outgoing unread flags never "
		   "admit own voice");
	expect(queue.next(0, table({heard, incoming})) == -1,
		   "first heard incoming item ends unheard queue");
	expect(!queue.permitsAdvance() && queue.next(0, table({incoming})) == -1,
		   "a heard boundary remains closed for later appends");
	queue.begin(settings, PlaybackChat::Other, false);
	expect(queue.resumesMusic() && queue.next(0, table({heard, incoming})) == 0,
		   "next manual begin captures new settings and opens the closed queue");
	queue.begin(Playback(), PlaybackChat::Other, false);
	expect(queue.next(0, table({own, ownUnheard, incoming})) == 2,
		   "skipped outgoing messages do not close an unheard queue");
	expect(queue.next(0, table({})) == -1 && !queue.permitsAdvance(),
		   "loaded-slice exhaustion ends continuation without a history fetch");
	queue.clear();
	expect(!queue.next(0, table({incoming})) && queue.permitsAdvance() &&
			   queue.resumesMusic(),
		   "lifetime clear removes the snapshot and restores native delegation");
	settings.afterVoice = VoiceContinuation::AllFollowing;
	settings.privateChatQueue = true;
	queue.begin(settings, PlaybackChat::OneToOne, false);
	auto visits = 0;
	expect(queue.next(std::numeric_limits<int>::max(),
					  [&](int) {
						  ++visits;
						  return own;
					  }) == -1 &&
			   visits == 1,
		   "index upper bound cannot overflow while skipping an own message");
	std::cout << "VOICE_CONTINUATION_BRIDGE_POLICY_CHECKS=" << checks
			  << " FAILURES=" << failures << '\n';
	return failures ? 1 : 0;
}

#include "voice-queue-bridge.inc"

int VoiceBridgeCases() {
	using namespace Ports;
	using AudioMsgId = Ports::AudioMsgId;
	using DocumentData = Ports::DocumentData;
	using PeerData = Ports::PeerData;
	using History = Ports::History;
	using HistoryItem = Ports::HistoryItem;
	using Type = AudioMsgId::Type;
	using Purple::VoiceContinuation;
	int checks = 0, failures = 0, baselineBad = 0;
	auto expect = [&](bool v, const char *name) {
		++checks;
		if (!v) {
			++failures;
			std::cerr << name << '\n';
		}
	};
	Ports::Main::Session sa, sb;
	PeerData peer;
	peer.id = 11;
	peer.user = true;
	History history{nullptr, &sa, &peer};
	HistoryItem a{&history, {11, 1}}, b{&history, {11, 2}}, c{&history, {11, 3}};
	Owner owner;
	history.o = &owner;
	owner.s = &sa;
	owner.items = {&a, &b, &c};
	DocumentData da{&owner}, db{&owner}, dc{&owner};
	AudioMsgId A(&da, a.id), B(&db, b.id), C(&dc, c.id);
	auto reset = [&] {
		Purple::StartVoiceQueue();
		Purple::Config.playback = Purple::Playback();
		OnIntent = {};
		OnShared = {};
		OnPause = {};
		OnRead = {};
		OnUpdated = {};
		OnFinished = {};
		OnTimer = {};
		TimerEnds = false;
		owner.streamingPort.available = true;
		owner.streamingPort.requests = 0;
		owner.reads = 0;
		a.outgoing = false;
		a.unread = true;
		b.outgoing = false;
		b.unread = true;
		c.outgoing = false;
		c.unread = true;
	};
	auto begin = [&](AudioMsgId id, bool interrupted) {
		Purple::VoicePlaybackAdmission call(id, nullptr);
		return call.commit(interrupted);
	};
	reset();
	Purple::Config.playback.afterVoice = VoiceContinuation::ResumeMusic;
	Purple::Config.playback.resumeMusicAfterVoice = false;
	expect(begin(A, true), "manual A commits");
	expect(!Purple::VoiceQueueMayAdvance(Type::Voice, true),
		   "resume music refuses automatic queue");
	Purple::Config.playback.afterVoice = VoiceContinuation::AllFollowing;
	Purple::Config.playback.resumeMusicAfterVoice = true;
	{
		Purple::VoiceAutomaticPlayGuard automatic(B, true);
		expect(begin(B, true), "automatic B admits");
	}
	expect(!Purple::VoiceQueueMayAdvance(Type::Voice, true) &&
			   !Purple::ResumeMusicAfterVoice(),
		   "automatic start retains captured settings");
	expect(begin(B, true), "same-item replay admits");
	expect(Purple::VoiceQueueMayAdvance(Type::Voice, true) &&
			   Purple::ResumeMusicAfterVoice(),
		   "same-item replay captures changed settings");
	Purple::Config.playback.afterVoice = VoiceContinuation::ResumeMusic;
	Purple::Config.playback.resumeMusicAfterVoice = false;
	expect(begin(A, false) && Purple::VoiceQueueMayAdvance(Type::Voice, true),
		   "without interrupted music Android parity keeps ordinary following queue");
	expect(!Purple::ResumeMusicAfterVoice(),
		   "without music override never changes independent resume preference");
	Purple::StopVoicePlaybackContinuation();
	expect(!Purple::VoiceQueueMayAdvance(Type::Voice, true) &&
			   !Purple::ResumeMusicAfterVoice(),
		   "Stop closes queue retaining captured resume-off");
	expect(Purple::VoiceQueueMayAdvance(Type::Voice, false) &&
			   Purple::VoiceQueueMayAdvance(Type::Song, true),
		   "manual and Song next remain independent");
	reset();
	expect(begin(A, true), "scope setup");
	b.outgoing = true;
	c.unread = true;
	auto at = [&](int i) -> HistoryItem * { return i == 1 ? &b : i == 2 ? &c : nullptr; };
	auto next = Purple::ConfinedVoiceNext(Type::Voice, true, &history, 0, 0, 1, at);
	expect(next && *next == &c, "actual incoming queue skips outgoing candidate");
	c.unread = false;
	next = Purple::ConfinedVoiceNext(Type::Voice, true, &history, 0, 0, 1, at);
	expect(next && !*next && !Purple::VoiceQueueMayAdvance(Type::Voice, true),
		   "heard incoming boundary closes actual bridge queue");
	expect(!Purple::ConfinedVoiceNext(Type::Voice, false, &history, 0, 0, 1, at),
		   "explicit next bypasses automatic filtering");
	reset();
	peer.forum = true;
	a.topic = 99;
	{
		Purple::VoicePlaybackAdmission call(A, nullptr);
		expect(call.commit(true), "forum admission");
	}
	expect(!Purple::ConfinedVoiceNext(Type::Voice, true, &history, 98, 0, 1, at),
		   "different topic cannot inherit policy");
	expect(
		Purple::ConfinedVoiceNext(Type::Voice, true, &history, 99, 0, 1, at).has_value(),
		"same normalized topic uses captured policy");
	peer.forum = false;
	a.topic = 0;
	peer.mono = true;
	a.mono = 33;
	expect(begin(A, true), "monoforum admission");
	expect(!Purple::ConfinedVoiceNext(Type::Voice, true, &history, 0, 34, 1, at),
		   "different monoforum sublist cannot inherit");
	expect(
		Purple::ConfinedVoiceNext(Type::Voice, true, &history, 0, 33, 1, at).has_value(),
		"same monoforum sublist uses capture");
	peer.mono = false;
	a.mono = 0;
	PeerData upgraded;
	upgraded.id = 55;
	History migrated{nullptr, &sa, &upgraded};
	history.normalized = &migrated;
	expect(begin(A, true), "migrated admission");
	expect(
		Purple::ConfinedVoiceNext(Type::Voice, true, &migrated, 0, 0, 1, at).has_value(),
		"native migrated-to scope matches policy");
	expect(!Purple::ConfinedVoiceNext(Type::Voice, true, &history, 0, 0, 1, at),
		   "raw migrated-from scope does not replace normalized scope");
	history.normalized = nullptr;
	reset();
	expect(begin(A, true), "stream setup");
	auto source = Purple::NewVoicePlaybackStreamGuard();
	auto old = Purple::VoicePlaybackContinuationGuard();
	Owner other;
	other.s = &sb;
	DocumentData otherDoc{&other};
	AudioMsgId otherId(&otherDoc, {22, 1});
	{
		Purple::VoicePlaybackAdmission failed(otherId, nullptr);
		expect(!source(), "old stream suppresses delivery while new admission pending");
	}
	invalidate_weak_ptrs(&sb);
	expect(source() && Purple::VoicePlaybackContinuationGuard()(),
		   "failed uncommitted admission restores retained stream session context");
	expect(!old(), "failed admission never revives old operation generation");
	auto transition = Purple::VoiceQueueTransitionGuard();
	auto native = Purple::VoicePlaybackContinuationGuard();
	Purple::NoteVoicePlaybackInteraction();
	expect(!transition() && native(), "Pause/Resume cancels automatic transition without "
									  "invalidating call/seek outer frame");
	Purple::RetireVoicePlaybackStream();
	expect(!source(), "retired stream callback cannot deliver");
	{
		auto dying = std::make_unique<Ports::Main::Session>();
		Owner transient;
		transient.s = dying.get();
		DocumentData doc{&transient};
		AudioMsgId id(&doc, {44, 1});
		expect(begin(id, true), "weak session start");
		auto current = Purple::VoicePlaybackContinuationGuard();
		dying.reset();
		expect(!current() && !Purple::ResumeMusicAfterVoice() &&
				   !Purple::VoiceQueueMayAdvance(Type::Voice, true),
			   "destroyed session invalidates continuation/resume");
	}
	reset();
	Purple::Config.playback.afterVoice = VoiceContinuation::ResumeMusic;
	begin(A, true);
	Purple::Config.playback.afterVoice = VoiceContinuation::AllFollowing;
	{
		Purple::VoiceAutomaticPlayGuard automatic(B, true);
		expect(begin(C, true), "different internal target admits as manual");
	}
	expect(Purple::VoiceQueueMayAdvance(Type::Voice, true),
		   "operation token matches exact document and context");
	reset();
	{
		Purple::VoiceAutomaticPlayGuard expired(B, true);
	}
	Purple::Config.playback.afterVoice = VoiceContinuation::ResumeMusic;
	begin(B, true);
	expect(!Purple::VoiceQueueMayAdvance(Type::Voice, true),
		   "unused automatic token expires at scope exit");
	// The exact extracted native bodies own the notification and admission order.
	reset();
	Baseline::Instance before;
	before.voice.current = A;
	before.voice.next = B;
	before._switchToNext.callback = [&] { before.play(C); };
	before.moveInItem(&before.voice, 1, true);
	baselineBad += before.voice.current == B;
	expect(
		before.voice.current == B,
		"single missing authority check reproduces overwriting callback-issued manual C");
	reset();
	Proposed::Instance after;
	after.voice.current = A;
	after.voice.next = B;
	after._switchToNext.callback = [&] { after.play(C); };
	after.moveInItem(&after.voice, 1, true);
	expect(after.voice.current == C && after.starts.size() == 1,
		   "new exact native move keeps callback-issued manual C");
	reset();
	Proposed::Instance paused;
	paused.voice.current = A;
	paused.voice.next = B;
	paused._switchToNext.callback = [] { Purple::NoteVoicePlaybackInteraction(); };
	paused.moveInItem(&paused.voice, 1, true);
	expect(paused.starts.empty(),
		   "notification Pause/Resume refuses old automatic start");
	reset();
	Proposed::Instance stopped;
	stopped.voice.current = A;
	stopped.voice.next = B;
	stopped._switchToNext.callback = [] { Purple::StopVoicePlaybackContinuation(); };
	stopped.moveInItem(&stopped.voice, 1, true);
	expect(stopped.starts.empty(), "notification Stop refuses old automatic start");
	for (int stage = 0; stage < 4; ++stage) {
		reset();
		Proposed::Instance p;
		auto callback = [&] { p.play(B); };
		if (stage == 0)
			OnIntent = callback;
		else if (stage == 1)
			OnShared = callback;
		else if (stage == 2)
			OnPause = callback;
		else
			OnRead = callback;
		p.play(A);
		expect(p.voice.current == B,
			   "new exact native play retains successor at every callback port");
		if (stage < 3)
			expect(p.starts.size() == 1,
				   "superseded native A never reaches stream publication");
		expect(p._playerStartedPlay.count == 1,
			   "superseded native A never publishes started-play event");
		expect(p._pendingContextFor == B,
			   "old admission never overwrites successor pending context");
	}
	reset();
	Proposed::Instance plain;
	plain.voice.current = A;
	plain.voice.next = B;
	plain.moveInItem(&plain.voice, 1, true);
	expect(plain.voice.current == B,
		   "ordinary automatic navigation still plays expected B");
	for (const auto state : {Proposed::State::Paused, Proposed::State::Pausing,
			Proposed::State::Stopped, Proposed::State::Stopping}) {
		reset();
		Proposed::Instance inactiveSong;
		inactiveSong.songState = state;
		inactiveSong.play(A);
		inactiveSong.resumeSongAfterVoice();
		expect(inactiveSong.pauses == 0 && inactiveSong.songResumes == 0,
			   "ending voice cannot start an earlier inactive song");
	}
	reset();
	Proposed::Instance replacement;
	replacement.play(A);
	expect(replacement.song.resumeOnVoiceEnd, "voice pauses an actually playing song");
	DocumentData newerSong{&owner, false, false, true};
	replacement.play(AudioMsgId(&newerSong, {}));
	replacement.songState = Proposed::State::Paused;
	replacement.resumeSongAfterVoice();
	expect(replacement.songResumes == 0, "starting music explicitly discards the old voice interruption");
	reset();
	Proposed::Instance interrupted;
	Purple::Config.playback.afterVoice = VoiceContinuation::Unheard;
	c.unread = false;
	OnPause = [&] { interrupted.play(B); };
	interrupted.play(A);
	auto nativeNext = Purple::ConfinedVoiceNext(
		Type::Voice, true, &history, 0, 0, 2,
		[&](int i) -> HistoryItem * { return i == 2 ? &c : nullptr; });
	expect(nativeNext && !*nativeNext, "native pause notification successor must retain "
									   "interrupted-music Unheard policy");
	for (bool resume : {false, true}) {
		reset();
		Proposed::Instance stoppedDuringPause;
		Purple::Config.playback.resumeMusicAfterVoice = resume;
		OnPause = [&] {
			Purple::StopVoicePlaybackContinuation();
			stoppedDuringPause.resumeSongAfterVoice();
		};
		stoppedDuringPause.play(A);
		expect(stoppedDuringPause.starts.empty(),
			   "Stop during music pause prevents voice stream start");
		expect(stoppedDuringPause.songResumes == (resume ? 1 : 0),
			   "Stop during music pause honors already-captured independent resume "
			   "preference");
		expect(!stoppedDuringPause.song.resumeOnVoiceEnd,
			   "stale outer pause cannot re-arm consumed native music flag");
	}
	reset();
	Proposed::Instance newSong;
	OnPause = [&] {
		Purple::AdmitPlaylistPlayerIntent(Type::Song, Purple::PlaylistPlayerIntent::Play);
		newSong.songState = Proposed::State::Stopped;
	};
	newSong.play(A);
	expect(!newSong.song.resumeOnVoiceEnd,
		   "external Song intent clears pending interruption before native work");
	newSong.resumeSongAfterVoice();
	expect(newSong.songResumes == 0,
		   "explicit newer Song intent is not restored as old interrupted music");
	bool nestedFlag = false;
	{
		Purple::VoiceMusicPauseGuard outer(nestedFlag, [] { return true; });
		{
			Purple::VoiceMusicPauseGuard inner(nestedFlag, [] { return true; });
		}
		Purple::ClearVoiceMusicPause();
		expect(!nestedFlag,
			   "nested pause scope restores its live parent cancellation claim");
		nestedFlag = true;
	}
	expect(nestedFlag, "cleared scopes cannot later mutate a newer native flag");
	bool staleFlag = true;
	{
		Purple::VoiceMusicPauseGuard stale(staleFlag, [] { return false; });
		Purple::ClearVoiceMusicPause();
	}
	expect(staleFlag, "expired Song authority prevents a late native-flag dereference");

	reset();
	expect(begin(A, true), "finish setup admits A");
	auto beforeFinish = Purple::VoicePlaybackContinuationGuard();
	auto streamBeforeFinish = Purple::NewVoicePlaybackStreamGuard();
	auto transitionBeforeFinish = Purple::VoiceQueueTransitionGuard();
	Purple::FinishVoiceQueue();
	expect(!beforeFinish() && !streamBeforeFinish() && !transitionBeforeFinish(),
		   "finish retires call transition and stream authorities");
	expect(!Purple::VoicePlaybackAvailable() && !Purple::ResumeMusicAfterVoice() &&
			   !Purple::VoiceQueueMayAdvance(Type::Voice, true),
		   "finished player cannot advance or restore music");
	expect(!Purple::NewVoicePlaybackStreamGuard()(),
		   "finished player cannot mint a new stream authority");
	expect(!begin(B, true), "direct finished admission cannot commit");
	DocumentData songDoc{&owner, false, false, true};
	AudioMsgId songId(&songDoc, {11, 9});
	{
		Purple::VoicePlaybackAdmission song(songId, nullptr);
		expect(song.commit(false),
			   "Voice finish state does not intercept Song admission");
	}
	{
		Purple::VoicePlaybackAdmission unknown(AudioMsgId(), nullptr);
		expect(unknown.commit(false),
			   "Voice finish state does not intercept non-Voice empty admission");
	}

	Proposed::Instance finished;
	finished.play(B);
	expect(finished.starts.empty() && owner.streamingPort.requests == 0,
		   "native play refuses finished Voice before stream lookup");
	Purple::FinishVoiceQueue();
	expect(!beforeFinish(), "repeated finish remains retired");
	Purple::StartVoiceQueue();
	expect(Purple::VoicePlaybackAvailable() && begin(B, true),
		   "restart opens fresh Voice authority");
	expect(!beforeFinish() && !streamBeforeFinish() && !transitionBeforeFinish(),
		   "restart never revives prior epoch authorities");
	expect(Purple::NewVoicePlaybackStreamGuard()(), "restart permits a fresh stream");
	reset();
	{
		Purple::VoicePlaybackAdmission pending(A, nullptr);
		Purple::FinishVoiceQueue();
		expect(!pending.commit(true), "finish during admission prevents late commit");
	}
	expect(!Purple::VoicePlaybackAvailable() &&
			   !Purple::VoicePlaybackContinuationGuard()(),
		   "admission destructor cannot reopen finished lifetime");
	for (int stage = 0; stage < 4; ++stage) {
		reset();
		Proposed::Instance p;
		auto callback = [] { Purple::FinishVoiceQueue(); };
		if (stage == 0)
			OnIntent = callback;
		else if (stage == 1)
			OnShared = callback;
		else if (stage == 2)
			OnPause = callback;
		else
			OnRead = callback;
		p.play(A);
		if (stage < 3)
			expect(p.starts.empty(),
				   "finish at admission callback prevents obsolete stream publication");
		expect(p._playerStartedPlay.count == 0,
			   "finish at callback prevents started-play publication");
		auto count = p.starts.size();
		p.play(B);
		expect(p.starts.size() == count, "callback-issued play after finish is refused");
	}

	auto preparePlaylist = [&](Proposed::Instance &p) {
		p.voice.current = A;
		p.voice.next = {};
		p.voice.history = &history;
		p.voice.playlistIndex = 0;
		p.voice.playlistSlice = Proposed::PlaylistSlice{{a.id, b.id, c.id}};
		p.songState = Proposed::State::Paused;
		p.song.resumeOnVoiceEnd = true;
	};
	ItemMedia ma{&da}, mb{&db}, mc{&dc};
	a.m = &ma;
	b.m = &mb;
	c.m = &mc;
	reset();
	Proposed::Instance playlist;
	preparePlaylist(playlist);
	begin(A, true);
	playlist._switchToNext.callback = [&] { playlist.play(C); };
	playlist.moveInPlaylist(&playlist.voice, 1, true);
	expect(playlist.voice.current == C && playlist.starts.size() == 1,
		   "playlist notification cannot overwrite manual successor");
	reset();
	Proposed::Instance heardEnd;
	preparePlaylist(heardEnd);
	begin(A, true);
	b.unread = false;
	expect(!heardEnd.moveInPlaylist(&heardEnd.voice, 1, true) && heardEnd.starts.empty(),
		   "native playlist path stops at first heard incoming item");
	reset();
	Proposed::Instance skipOwn;
	preparePlaylist(skipOwn);
	begin(A, true);
	b.outgoing = true;
	skipOwn.moveInPlaylist(&skipOwn.voice, 1, true);
	expect(skipOwn.voice.current == C,
		   "native playlist path skips own message and plays next incoming");
	reset();
	Proposed::Instance ttl;
	preparePlaylist(ttl);
	begin(A, true);
	mb.ttl = 1;
	expect(!ttl.moveInPlaylist(&ttl.voice, 1, true) && ttl.starts.empty(),
		   "native playlist retains TTL refusal");
	mb.ttl = 0;
	reset();
	Proposed::Instance explicitNext;
	preparePlaylist(explicitNext);
	Purple::Config.playback.afterVoice = VoiceContinuation::ResumeMusic;
	begin(A, true);
	expect(!explicitNext.moveInPlaylist(&explicitNext.voice, 1, true),
		   "Resume music mode blocks native automatic next");
	expect(explicitNext.moveInPlaylist(&explicitNext.voice, 1, false) &&
			   explicitNext.voice.current == B,
		   "explicit native Next remains independent of captured automatic policy");
	reset();
	Proposed::Instance end;
	preparePlaylist(end);
	begin(A, true);
	b.unread = false;
	end.voiceState = Proposed::State::StoppedAtEnd;
	end.emitUpdate(Type::Voice, [](const auto &) { return true; });
	expect(
		end._tracksFinished.count == 1 && end.songResumes == 1 && end.starts.empty(),
		"natural heard boundary restores interrupted music after finished notification");
	reset();
	Proposed::Instance sleep;
	preparePlaylist(sleep);
	begin(A, true);
	sleep.voiceState = Proposed::State::StoppedAtEnd;
	TimerEnds = true;
	sleep.emitUpdate(Type::Voice, [](const auto &) { return true; });
	expect(
		sleep._tracksFinished.count == 1 && sleep.songResumes == 0 &&
			sleep.starts.empty(),
		"sleep timer finishes before automatic continuation and suppresses music resume");
	reset();
	Proposed::Instance updated;
	preparePlaylist(updated);
	begin(A, true);
	updated.voiceState = Proposed::State::StoppedAtEnd;
	OnUpdated = [&] { updated.play(C); };
	updated.emitUpdate(Type::Voice, [](const auto &) { return true; });
	expect(updated.voice.current == C && updated._tracksFinished.count == 0,
		   "updated notification successor invalidates stale end continuation");
	reset();
	Proposed::Instance finishedCallback;
	preparePlaylist(finishedCallback);
	begin(A, true);
	b.unread = false;
	finishedCallback.voiceState = Proposed::State::StoppedAtEnd;
	OnFinished = [&] { finishedCallback.play(C); };
	finishedCallback.emitUpdate(Type::Voice, [](const auto &) { return true; });
	expect(finishedCallback.voice.current == C && finishedCallback.songResumes == 0,
		   "finished notification manual successor prevents stale music restore");
	reset();
	Proposed::Instance finishInEnd;
	preparePlaylist(finishInEnd);
	begin(A, true);
	b.unread = false;
	finishInEnd.voiceState = Proposed::State::StoppedAtEnd;
	OnFinished = [] { Purple::FinishVoiceQueue(); };
	finishInEnd.emitUpdate(Type::Voice, [](const auto &) { return true; });
	expect(finishInEnd.songResumes == 0 && !Purple::VoicePlaybackAvailable(),
		   "finish during native end notification retires music restore authority");
	std::cout << "VOICE_BRIDGE_LIFECYCLE_CHECKS=" << checks << " FAILURES=" << failures
			  << " BASELINE_REPRODUCTIONS=" << baselineBad << '\n';
	return failures ? 1 : 0;
}

int main() {
	if (const auto result = OriginalVoiceQueueCases())
		return result;
	if (const auto result = ContinuationPolicyCases())
		return result;
	return VoiceBridgeCases();
}

#include "purple/purple_voice_queue_rules.h"

#include <iostream>
#include <vector>

int main() {
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

	std::cout << (checks - failures) << '/' << checks << " voice queue checks passed\n";
	return failures ? 1 : 0;
}

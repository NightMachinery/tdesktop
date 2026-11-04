#include "purple/purple_media_rules.h"

#include <cstdint>
#include <iostream>

int main() {
	auto checks = 0;
	auto failures = 0;
	const auto expect = [&](bool value, const char *name) {
		++checks;
		if (!value) { ++failures; std::cerr << name << '\n'; }
	};
	using namespace Purple::MediaRules;
	const auto playing = RestartFacts{
		.leftButton = true,
		.documentOpen = true,
		.voiceOrRound = true,
		.currentTrack = true,
		.seekable = true,
	};
	const auto declines = [&](RestartFacts facts) {
		return RestartOnDoubleClick(facts) == Restart::None;
	};
	expect(RestartOnDoubleClick(playing) == Restart::Seek, "the playing voice or round seeks to 0:00");

	auto facts = playing;
	facts.seekable = false;
	expect(RestartOnDoubleClick(facts) == Restart::Replay, "a track not streaming yet is played again from the start");

	facts = playing;
	facts.leftButton = false;
	expect(declines(facts), "another button declines");

	facts = playing;
	facts.selecting = true;
	expect(declines(facts), "selecting messages declines");

	facts = playing;
	facts.documentOpen = false;
	expect(declines(facts), "a handler that does not open a document declines");

	facts = playing;
	facts.voiceOrRound = false;
	expect(declines(facts), "a document that is not voice or round declines");

	facts = playing;
	facts.viewOnce = true;
	expect(declines(facts), "view-once media declines");

	facts = playing;
	facts.currentTrack = false;
	expect(declines(facts), "a message that is not the current track declines");

	facts.seekable = false;
	expect(declines(facts), "not current and not streaming declines");

	expect(declines(RestartFacts()), "nothing set declines");

	using Context = std::int64_t;
	expect(ResolveRestartContext(Context(0), true, Context(42)) == 42, "a seek link without a context takes the current track's");
	expect(ResolveRestartContext(Context(0), false, Context(42)) == 0, "a link without a context for another document stays empty");
	expect(ResolveRestartContext(Context(7), true, Context(42)) == 7, "a link with its own context keeps it");
	expect(ResolveRestartContext(Context(7), false, Context(42)) == 7, "a context is never replaced");
	expect(ResolveRestartContext(Context(0), true, Context(0)) == 0, "nothing current stays empty");

	std::cout << (checks - failures) << '/' << checks << " media restart checks passed\n";
	return failures ? 1 : 0;
}

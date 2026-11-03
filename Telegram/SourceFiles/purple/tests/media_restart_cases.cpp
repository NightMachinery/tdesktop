#include "purple/purple_media_rules.h"

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
	expect(RestartsOnDoubleClick(playing), "the playing voice or round restarts");

	auto facts = playing;
	facts.leftButton = false;
	expect(!RestartsOnDoubleClick(facts), "another button declines");

	facts = playing;
	facts.selecting = true;
	expect(!RestartsOnDoubleClick(facts), "selecting messages declines");

	facts = playing;
	facts.documentOpen = false;
	expect(!RestartsOnDoubleClick(facts), "a handler that does not open a document declines");

	facts = playing;
	facts.voiceOrRound = false;
	expect(!RestartsOnDoubleClick(facts), "a document that is not voice or round declines");

	facts = playing;
	facts.viewOnce = true;
	expect(!RestartsOnDoubleClick(facts), "view-once media declines");

	facts = playing;
	facts.currentTrack = false;
	expect(!RestartsOnDoubleClick(facts), "a message that is not the current track declines");

	facts = playing;
	facts.seekable = false;
	expect(!RestartsOnDoubleClick(facts), "a track not yet streamed declines");

	expect(!RestartsOnDoubleClick(RestartFacts()), "nothing set declines");

	std::cout << (checks - failures) << '/' << checks << " media restart checks passed\n";
	return failures ? 1 : 0;
}

#include "purple/purple_seek_rules.h"

#include <cstdint>
#include <iostream>

int main() {
	auto checks = 0;
	auto failures = 0;
	const auto expect = [&](bool value, const char *name) {
		++checks;
		if (!value) { ++failures; std::cerr << name << '\n'; }
	};
	using namespace Purple::SeekRules;
	using ms = std::int64_t;

	expect(StepFrom(5) == 5 && StepFrom(10) == 10 && StepFrom(15) == 15 && StepFrom(30) == 30, "every listed step is kept");
	expect(StepFrom(0) == 10 && StepFrom(3) == 10 && StepFrom(60) == 10 && StepFrom(-10) == 10, "any other step reads as 10");
	expect(kStepDefault == 10, "the default step is 10 s");
	expect(StepSetting(5) == "5s" && StepSetting(30) == "30s", "the step is written the way settings.toml spells it");
	expect(StepSetting(7) == "10s", "an unknown step is written as the default");

	expect(SeekTarget(20'000, 60'000, 1, 10) == ms(30'000), "forward moves by the step");
	expect(SeekTarget(20'000, 60'000, -1, 10) == ms(10'000), "back moves by the step");
	expect(SeekTarget(20'000, 60'000, 1, 5) == ms(25'000), "a 5 s step");
	expect(SeekTarget(20'000, 60'000, -1, 15) == ms(5'000), "a 15 s step");
	expect(SeekTarget(40'000, 120'000, 1, 30) == ms(70'000), "a 30 s step");
	expect(SeekTarget(20'000, 60'000, 1, 7) == ms(30'000), "an unknown step seeks by 10 s");

	expect(SeekTarget(4'000, 60'000, -1, 10) == ms(0), "back before the start goes to 0:00");
	expect(!SeekTarget(0, 60'000, -1, 10), "back at 0:00 does nothing");
	expect(SeekTarget(55'000, 60'000, 1, 10) == ms(59'000), "forward past the end goes to one second before it");
	expect(!SeekTarget(59'000, 60'000, 1, 10), "forward at one second before the end does nothing");
	expect(!SeekTarget(59'500, 60'000, 1, 10), "forward inside the last second never moves back");
	expect(SeekTarget(59'500, 60'000, -1, 10) == ms(49'500), "back from the last second still works");
	expect(!SeekTarget(300, 800, 1, 10), "forward in a track shorter than a second does nothing");
	expect(SeekTarget(300, 800, -1, 10) == ms(0), "back in a track shorter than a second goes to 0:00");

	expect(!SeekTarget(10'000, 0, 1, 10), "an unknown length does nothing");
	expect(!SeekTarget(10'000, -1, -1, 10), "a negative length does nothing");
	expect(!SeekTarget(10'000, 60'000, 0, 10), "no direction does nothing");
	expect(SeekTarget(-500, 60'000, 1, 10) == ms(10'000), "a position before 0 is read as 0");
	expect(SeekTarget(70'000, 60'000, -1, 10) == ms(50'000), "a position past the end is read as the end");

	expect(ToMs(48'000, 48'000) == ms(1'000) && ToMs(1'500, 1'000) == ms(1'500), "samples and milliseconds convert");
	expect(ToMs(0x7FFF'FFFF'FFFF'FFFFLL, 1'000) == std::nullopt, "an unavailable duration is unknown, not an overflow");
	expect(ToMs(0x7FFF'FFFF'FFFF'FFFFLL, 48'000) == std::nullopt, "an unavailable sample count is unknown, not an overflow");
	expect(ToMs(kLongestMs, 1'000) == ms(kLongestMs), "the longest supported length still converts");
	expect(!ToMs(-1, 1'000) && !ToMs(1'000, 0), "a negative value or no frequency is unknown");

	const auto playing = SeekFacts{
		.current = true,
		.position = 20'000,
		.length = 60'000,
		.frequency = 1'000,
	};
	auto facts = playing;
	auto plan = PlanSeek(facts, -1, 10);
	expect(plan.action == SeekAction::SeekPlaying && plan.progress == 10'000. / 60'000., "a playing track seeks and keeps playing");
	facts.paused = true;
	plan = PlanSeek(facts, 1, 10);
	expect(plan.action == SeekAction::SeekPaused && plan.progress == 0.5, "a paused track seeks and stays paused");
	facts = playing;
	facts.current = false;
	expect(PlanSeek(facts, 1, 10).action == SeekAction::Decline, "a track that is not current is declined");
	facts = playing;
	facts.length = 0;
	expect(PlanSeek(facts, -1, 10).action == SeekAction::Decline, "an unknown length is declined");
	facts.length = 0x7FFF'FFFF'FFFF'FFFFLL;
	expect(PlanSeek(facts, -1, 10).action == SeekAction::Decline, "an unavailable length is declined");
	facts = playing;
	facts.stopped = true;
	facts.position = 0;
	plan = PlanSeek(facts, -1, 10);
	expect(plan.action == SeekAction::SeekPlaying && plan.progress == 50'000. / 60'000., "back after the end replays the last step");
	facts.paused = true;
	expect(PlanSeek(facts, -1, 10).action == SeekAction::SeekPlaying, "back after the end plays even if it was paused");
	expect(PlanSeek(facts, 1, 10).action == SeekAction::Consume, "forward after the end does nothing but keeps the key");
	facts = playing;
	facts.position = 0;
	expect(PlanSeek(facts, -1, 10).action == SeekAction::Consume, "back at 0:00 does nothing but keeps the key");

	expect(SeekShortcutKeys(-1, false) == "alt+shift+left" && SeekShortcutKeys(1, false) == "alt+shift+right", "Windows and Linux keep alt+shift and an arrow");
	expect(SeekShortcutKeys(-1, true) == "ctrl+alt+left" && SeekShortcutKeys(1, true) == "ctrl+alt+right", "macOS uses Command+Option and an arrow");
	expect(SeekShortcutKeys(-1, true).find("shift") == std::string::npos, "the macOS keys are not Option+Shift word selection");

	std::cout << (checks - failures) << '/' << checks << " recording cases passed\n";
	return failures ? 1 : 0;
}

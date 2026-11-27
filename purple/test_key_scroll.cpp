#include "purple/purple_key_scroll.h"

#include <iostream>

namespace {

using Purple::KeyScroll::Motion;

int Checks = 0;
int Failures = 0;

void Check(bool passed, const char *what) {
	++Checks;
	if (!passed) {
		++Failures;
		std::cerr << "FAIL: " << what << '\n';
	}
}

[[nodiscard]] int HoldAtRate(int rate) {
	auto motion = Motion();
	auto position = 1000;
	motion.startHold(0, 1);
	for (auto i = 1; i <= rate; ++i) {
		position = motion.advance(i * 1000 / rate, position, 5000);
	}
	return position;
}

[[nodiscard]] int ReleaseBetweenFrames(int rate) {
	auto motion = Motion();
	auto position = 1000;
	motion.startHold(0, 1);
	for (auto frame = 1; frame * 1000 / rate < 503; ++frame) {
		position = motion.advance(frame * 1000 / rate, position, 5000);
	}
	motion.release(503);
	for (auto now = 510; now <= 590; now += 10) {
		position = motion.advance(now, position, 5000);
	}
	return position;
}

} // namespace

int main() {
	auto motion = Motion();
	auto position = 1000;
	Check(!motion.active(), "idle has no frames");
	motion.step(100, position, 5000, 84);
	Check(motion.active(), "tap starts a frame");
	position = motion.advance(141, position, 5000);
	Check(position > 1000 && position < 1084, "tap is eased");
	motion.release(145);
	position = motion.advance(210, position, 5000);
	Check(position == 1084 && !motion.active(), "released tap completes exactly one step");

	Check(HoldAtRate(60) == HoldAtRate(120), "60 and 120 Hz cover the same distance");
	Check(HoldAtRate(120) == 2330, "100 ms ramp and 1400 logical pixels per second");
	Check(ReleaseBetweenFrames(60) == ReleaseBetweenFrames(120),
		"release between frames preserves distance independently of refresh rate");
	motion.startHold(300, 1);
	for (auto t = 301; t <= 400; ++t) {
		position = motion.advance(t, position, 5000);
	}
	Check(position == 1154, "subpixel motion is carried forward at high refresh rates");
	motion.startHold(400, 1);
	position = motion.advance(410, position, 5000);
	Check(position == 1168, "OS repeat ticks do not restart the ramp");

	motion.release(410);
	const auto released = position;
	position = motion.advance(450, position, 5000);
	Check(position > released, "release eases out briefly");
	position = motion.advance(490, position, 5000);
	Check(position == released + 56 && !motion.active(), "ease-out ends after 80 ms");
	Check(motion.advance(550, position, 5000) == position, "no movement after braking");

	motion.startHold(600, 1);
	position = motion.advance(610, position, 5000);
	motion.step(610, position, 5000, -84);
	const auto reversed = motion.advance(630, position, 5000);
	Check(reversed < position, "opposite first press reverses immediately");
	motion.startHold(650, -1);
	Check(motion.advance(680, reversed, 5000) < reversed, "hold continues in the new direction");

	motion.startHold(700, -1);
	Check(motion.advance(740, 1, 5000) == 0 && !motion.active(), "top stops the frame");
	motion.startHold(700, 1);
	Check(motion.advance(740, 4999, 5000) == 5000 && !motion.active(), "bottom stops the frame");

	motion.step(800, 1000, 5000, 500);
	position = motion.advance(832, 1000, 5000);
	motion.step(832, position, 5000, 500);
	const auto retarget = position + 500;
	Check(motion.advance(940, position, 5000) == retarget && !motion.active(),
		"page repeats retarget from the current pixel without queueing a previous page");

	motion.startHold(1000, 1);
	Check(motion.advance(1250, 1000, 5000) == 1056, "stalled frames drop older movement");
	motion.cancel();
	Check(!motion.active() && motion.advance(1300, 1056, 5000) == 1056, "scope cancel stops now");

	auto boosted = Motion();
	position = 1000;
	boosted.startHold(0, 1);
	for (auto now = 10; now <= 100; now += 10) {
		position = boosted.advance(now, position, 5000);
	}
	boosted.setBoost(105, true);
	Check(boosted.advance(110, position, 5000) == 1091,
		"Shift applies at its event time without restarting the ramp");
	position = boosted.advance(120, 1091, 5000);
	Check(position == 1119, "Shift doubles the held velocity");
	boosted.setBoost(125, false);
	position = boosted.advance(130, position, 5000);
	Check(position == 1140, "releasing Shift returns to normal at once");
	boosted.release(130);
	position = boosted.advance(170, position, 5000);
	position = boosted.advance(210, position, 5000);
	Check(position == 1196 && !boosted.active(), "braking uses the speed at key release");

	std::cout << Checks << " checks, " << Failures << " failures\n";
	return Failures ? 1 : 0;
}

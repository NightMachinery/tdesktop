#include "purple/purple_key_repeat.h"

#include <iostream>

int main() {
	using namespace Purple::KeyRepeat;
	auto checks = 0;
	auto failures = 0;
	const auto expect = [&](bool pass, const char *name) {
		++checks;
		if (!pass) {
			++failures;
			std::cerr << "FAIL: " << name << '\n';
		}
	};

	enum class Action { Up, Down, ChatNext };
	using Hold = HeldMotion<Action>;
	const auto focus = int();
	const auto other = int();
	const auto scope = int();
	const auto target = Context{ &focus, &scope, true };
	const auto press = Stroke{ 38, 'J', 100, 1000 };
	auto repeat = Stroke{ 38, 'J', 600, 1500 };

	expect(CanArm(Policy::SameTarget, true, 1, false), "single Normal motion");
	expect(!CanArm(Policy::Never, true, 1, false), "single-shot action");
	expect(!CanArm(Policy::Count, true, 1, false), "countable action may still refuse holds");
	expect(!CanArm(Policy::SameTarget, false, 1, false), "Insert mode");
	expect(!CanArm(Policy::SameTarget, true, 2, false), "multi-stroke binding");
	expect(!CanArm(Policy::SameTarget, true, 1, true), "describe-key capture");

	auto held = Hold();
	expect(!held.next(repeat, target), "orphan repeat is ignored");
	held.start(press, target, Action::Down, Policy::SameTarget);
	expect(held.next(repeat, target) == Action::Down, "repeat the initial action");
	expect(!held.next(repeat, target), "coalesce events in the same delivery interval");
	repeat.source += 30;
	repeat.arrived += 30;
	expect(held.next(repeat, target) == Action::Down, "ordinary keyboard cadence");
	held.release(press.physical + 1);
	repeat.source += 30;
	repeat.arrived += 30;
	expect(held.next(repeat, target) == Action::Down, "other release preserves motion");
	held.release(press.physical);
	expect(!held.next(repeat, target), "release cancels immediately");

	held.start(press, target, Action::Down, Policy::SameTarget);
	auto stale = repeat;
	stale.arrived += 160;
	expect(!held.next(stale, target), "discard a queued event after a slow paint");
	repeat.source += 200;
	repeat.arrived += 200;
	expect(held.next(repeat, target) == Action::Down, "new events resume at normal cadence");
	auto remapped = repeat;
	remapped.chord = 'K';
	expect(!held.next(remapped, target), "modifier or logical binding changed");
	expect(!held.next(repeat, target), "changed chord cannot rearm itself");

	held.start(press, target, Action::Down, Policy::SameTarget);
	expect(!held.next(repeat, { &other, &scope, true }), "different widget");
	expect(!held.next(repeat, target), "returning to the widget needs a fresh press");
	held.start(press, target, Action::Down, Policy::SameTarget);
	expect(!held.next(repeat, { &focus, &other, true }), "different layer or controller");
	held.start(press, target, Action::Down, Policy::SameTarget);
	expect(!held.next(repeat, { &focus, &scope, false }), "mode change");
	held.start(press, target, Action::Down, Policy::SameTarget);
	held.cancel(true);
	expect(!held.next(repeat, target), "ordinary scroll stops on chat switch");
	held.start(press, target, Action::ChatNext, Policy::ChatNavigation);
	held.cancel(true);
	expect(held.next(repeat, target) == Action::ChatNext, "chat navigation can cross a chat switch");
	held.cancel();
	expect(!held.next(repeat, target), "every navigation stops on focus or protection change");

	std::cout << checks << " checks, " << failures << " failures\n";
	return failures ? 1 : 0;
}

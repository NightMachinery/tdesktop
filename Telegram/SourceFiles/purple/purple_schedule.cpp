/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_schedule.h"

#include "base/timer.h"
#include "purple/purple_config.h"
#include "purple/purple_device.h"
#include "purple/purple_engine.h"

#include <QtCore/QDateTime>

namespace Purple {
namespace {

// The schedule has minute resolution, so this is how late a boundary can be.
// Computing the exact moment of the next one and sleeping until it would be
// tidier and would then have to survive every way the wall clock can move
// underneath it - a laptop waking, a timezone change, the DST hour. Re-reading
// the clock on a cheap tick survives all of them by construction.
constexpr auto kTick = crl::time(30 * 1000);

class Runner final {
public:
	Runner();

private:
	void tick();

	base::Timer _timer;
	rpl::lifetime _lifetime;

};

Runner::Runner() {
	_timer.setCallback([=] { tick(); });
	_timer.callEach(kTick);

	// A settings reload can add, move or remove a rule, and a state write can
	// pause the schedule or move the preset out from under it. Either way the
	// answer can differ now rather than in thirty seconds.
	rpl::merge(
		SettingsChanges(),
		StateChanges()
	) | rpl::on_next([=] {
		tick();
	}, _lifetime);

	tick();
}

void Runner::tick() {
	const auto now = QDateTime::currentDateTime();
	if (ScheduleUnpauseDue(CurrentState(), now.toSecsSinceEpoch())) {
		// A pause given a deadline lifts itself, and then this same tick runs
		// the ordinary boundary rule below. That is what catches up, once and
		// immediately, on the windows that opened and closed while it was
		// paused - waiting for the next window edge instead would leave the
		// preset wherever the pause found it, possibly for a day.
		UpdateState([](State &state) {
			state.schedulePaused = false;
			state.schedulePausedUntil = 0;
		});
	}
	const auto &state = CurrentState();
	if (state.schedulePaused) {
		return;
	}
	const auto target = ScheduleTarget(
		ActiveSettings().schedule,
		now,
		ThisDevice());
	if (!target || *target == state.scheduleTarget) {
		// Acting on the change rather than on the value is the whole design.
		// It is what lets a preset chosen by hand stand until the next
		// boundary instead of being overwritten on the next tick, and what
		// makes a boundary missed while the app was closed still happen, once,
		// at the next launch.
		return;
	}
	const auto wanted = *target;
	const auto source = state.activeSource;
	const auto active = state.activePreset;
	const auto apply = (source != PresetSource::Focus)
		&& ScheduleApplies(
			ActiveSettings().schedule,
			ThisDevice(),
			wanted,
			source);

	// Two rules, and the asymmetry between them is deliberate. A window
	// starting is a positive instruction - "at nine, work mode" - and it
	// overrides a preset chosen by hand. A window ending only means the reason
	// for that preset has passed, which is no reason at all to undo something
	// asked for. Focus is left alone in both directions: it is the more
	// immediate signal, and a schedule fighting it would make both unreadable.
	//
	// The second clause is the core's now, because "a window ending" stopped
	// being "the target is Normal" once the preset between windows became a
	// key: five o'clock aiming at Home is a window ending too, and must not
	// steamroll a preset chosen by hand either. Which preset that is depends
	// on the rulesets this device runs, so the device goes with the question.
	UpdateState([&](State &state) {
		state.scheduleTarget = wanted;
		if (apply) {
			state.activePreset = wanted;
			state.activeSource = PresetSource::Schedule;
		}
	});
	LOG(("Purple: schedule wants '%1'%2."
		).arg(wanted
		).arg(apply
			? QString()
			: u", keeping '%1' (%2)"_q.arg(active, PresetSourceName(source))));
}

// Never destroyed, for the same reason as the config singleton: it holds an
// rpl subscription to something with static storage duration.
[[nodiscard]] Runner &Instance() {
	static const auto result = new Runner();
	return *result;
}

} // namespace

void StartSchedule() {
	Instance();
}

bool SchedulePaused() {
	return CurrentState().schedulePaused;
}

void SetSchedulePaused(bool paused, int64 until) {
	UpdateState([&](State &state) {
		state.schedulePaused = paused;

		// Both fields, always. A deadline left behind by a pause that has
		// since been lifted would expire under the next one and cut it short.
		state.schedulePausedUntil = paused ? until : 0;
	});
}

bool ScheduleConfigured() {
	// Every ruleset the file describes, including the implicit one the parser
	// wraps a flat [[schedule.rules]] array in - so this is still "the file
	// says something about a schedule", the way it always was, and a file
	// written before rulesets existed answers the same as before.
	//
	// Deliberately not narrowed to the rulesets that run on this device. A
	// ruleset written for the phone is a schedule the user is in the middle of
	// editing, and a pause row that vanished from the laptop while they wrote
	// it would read as the file having broken.
	return !ActiveSettings().schedule.rulesets.empty();
}

} // namespace Purple

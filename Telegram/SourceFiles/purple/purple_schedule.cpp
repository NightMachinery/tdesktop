/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_schedule.h"

#include "base/timer.h"
#include "lang/lang_keys.h"
#include "purple/purple_config.h"
#include "purple/purple_device.h"
#include "purple/purple_engine.h"

#include <QtCore/QDateTime>
#include <QtCore/QLocale>

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

// When the next window opens, so the line between windows can say how long the
// preset in force has left. The engine has no such helper and should not: it
// answers "what is true now", which is all a tick needs, and a boundary in the
// future is only ever wanted by something with a screen.
//
// A schedule repeats weekly, so the search is bounded: each rule's start, on
// each day it names, at its next occurrence from now. Nothing here worries
// about which rule would actually win at that moment - the earliest start is
// the earliest moment the answer can change, which is when this line is due to
// be rewritten anyway.
[[nodiscard]] std::optional<QDateTime> NextWindowStart(
		const ScheduleForDevice &active,
		const QDateTime &now) {
	auto result = std::optional<QDateTime>();
	const auto today = now.date().dayOfWeek();
	for (const auto pointer : active.rules) {
		const auto &rule = *pointer;
		for (const auto day : rule.days) {
			const auto ahead = (day - today + 7) % 7;
			const auto start = now.date().addDays(ahead).startOfDay();
			if (!start.isValid()) {
				// A date whose midnight does not exist, which is a real thing
				// in the timezones that move the clock at midnight. Skipping it
				// costs one candidate out of seven and keeps every comparison
				// below between two valid moments.
				continue;
			}
			auto when = start.addSecs(rule.from * 60);
			if (when <= now) {
				when = when.addDays(7);
			}
			if (!result || when < *result) {
				result = when;
			}
		}
	}
	return result;
}

// "09:00" for a window opening later today, "Mon 09:00" for one that is not. A
// bare time three days out would read as three hours out.
//
// QLocale rather than the core's WeekdayName(), which answers "mon" because it
// is the spelling settings.toml uses. That is the right answer for a file and
// the wrong one for a sentence.
[[nodiscard]] QString WindowStartText(
		const QDateTime &start,
		const QDateTime &now) {
	const auto time = start.time();
	const auto text = TimeOfDayText(time.hour() * 60 + time.minute());
	return (start.date() == now.date())
		? text
		: u"%1 %2"_q.arg(
			QLocale().dayName(start.date().dayOfWeek(), QLocale::ShortFormat),
			text);
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

QString PresetDisplayName(
		const Settings &settings,
		const QString &name) {
	if (name == NormalPreset()) {
		return u"Normal"_q;
	}
	const auto preset = settings.preset(name);
	return preset ? PresetTitle(*preset) : DefaultViewName(name);
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

QString ScheduleStatusText() {
	if (!ScheduleConfigured()) {
		return QString();
	}
	const auto &state = CurrentState();
	if (state.schedulePaused) {
		return state.schedulePausedUntil
			? u"Schedule paused until %1"_q.arg(langDateTime(
				QDateTime::fromSecsSinceEpoch(state.schedulePausedUntil)))
			: u"Schedule paused"_q;
	}
	const auto &settings = ActiveSettings();
	if (!settings.schedule.enabled) {
		return u"Schedule: switched off in the file"_q;
	}
	const auto now = QDateTime::currentDateTime();
	const auto active = ActiveSchedule(settings.schedule, ThisDevice());
	if (active.rules.empty()) {
		return u"Schedule: no rules for this device"_q;
	} else if (const auto rule = ScheduleRuleNow(active, now)) {
		return u"Schedule: %1 until %2, then %3"_q.arg(
			PresetDisplayName(settings, rule->preset),
			TimeOfDayText(rule->till),
			PresetDisplayName(settings, active.outside));
	}
	const auto next = NextWindowStart(active, now);
	return next
		? u"Schedule: %1 until %2"_q.arg(
			PresetDisplayName(settings, active.outside),
			WindowStartText(*next, now))
		: u"Schedule: %1"_q.arg(PresetDisplayName(settings, active.outside));
}

QString ScheduleDeviceText() {
	const auto &device = ThisDevice();
	return device.id.isEmpty()
		? u"This device reports no id, so rulesets naming one skip it."_q
		: u"(this device: %1)"_q.arg(DeviceLabelText(device.id));
}

} // namespace Purple

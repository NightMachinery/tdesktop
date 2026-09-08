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
	// Every word of the policy is in the core - the pause that lifts itself and
	// then catches up in the same pass, and the asymmetry where a window
	// starting overrides a preset chosen by hand while a window ending does
	// not. See ScheduleStep(). It was written here and in the Android bridge,
	// twice, with the same comments copied between them and a test on neither.
	const auto step = ScheduleStep(
		ActiveSettings(),
		CurrentState(),
		QDateTime::currentDateTime(),
		ThisDevice());
	if (!step) {
		return;
	}

	// The whole state, not field by field: what came back is a copy of the very
	// CurrentState() handed in a line ago, and both run on the UI thread, so
	// there is nothing in between for this to overwrite.
	UpdateState([&](State &state) {
		state = step->state;
	});
	if (step->unpaused) {
		LOG(("Purple: the schedule pause ran out."));
	}
	if (step->target.isEmpty()) {
		// Which happens on the one step that lifted a pause with no window to
		// catch up on. Saying the schedule wants '' would be a line about
		// nothing.
		return;
	}
	LOG(("Purple: schedule wants '%1'%2."
		).arg(step->target
		).arg(step->applied
			? QString()
			: u", keeping '%1' (%2)"_q.arg(
				step->kept,
				PresetSourceName(step->keptSource))));
}

// The schedule as it stands right now, asked of the core.
//
// A free function rather than a cached value: both callers below are drawing a
// line for somebody looking at it, they are called on a change or once a
// minute, and the answer is a walk over a handful of rules. Caching it would
// only add a thing that can be stale.
[[nodiscard]] ScheduleStatus Status() {
	return ScheduleStatusNow(
		ActiveSettings(),
		CurrentState(),
		QDateTime::currentDateTime(),
		ThisDevice());
}

// "09:00" for a window opening later today, "Mon 09:00" for one that is not. A
// bare time three days out would read as three hours out.
//
// QLocale rather than the core's WeekdayName(), which answers "mon" because it
// is the spelling settings.toml uses. That is the right answer for a file and
// the wrong one for a sentence.
[[nodiscard]] QString WindowStartText(int64 startUnix, const QDateTime &now) {
	const auto start = QDateTime::fromSecsSinceEpoch(startUnix);
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
	// "The file says something about a schedule at all", and the reasoning for
	// that - in particular why it is deliberately not narrowed to the rulesets
	// this device runs - is on ScheduleStatusNow() in the core now, along with
	// the sentence that answers the narrower question instead.
	return Status().kind != ScheduleStatusKind::NotConfigured;
}

QString ScheduleStatusText() {
	const auto &settings = ActiveSettings();
	const auto status = Status();
	const auto name = [&](const QString &preset) {
		return PresetDisplayName(settings, preset);
	};
	switch (status.kind) {
	case ScheduleStatusKind::NotConfigured:
		return QString();
	case ScheduleStatusKind::Paused:
		return u"Schedule paused"_q;
	case ScheduleStatusKind::PausedUntil:
		return u"Schedule paused until %1"_q.arg(langDateTime(
			QDateTime::fromSecsSinceEpoch(status.pausedUntil)));
	case ScheduleStatusKind::Off:
		return u"Schedule: switched off in the file"_q;
	case ScheduleStatusKind::NoRules:
		return u"Schedule: no rules yet"_q;
	case ScheduleStatusKind::NoneHere:
		// Told apart from the line above because the core tells them apart, and
		// they are different things to be told: a file with nothing in it, and
		// a file with nothing in it for here. With rulesets the second is what
		// a laptop holding only the phone's schedule goes on seeing.
		return u"Schedule: no rules for this device"_q;
	case ScheduleStatusKind::InsideWindow:
		return u"Schedule: %1 until %2, then %3"_q.arg(
			name(status.preset),
			TimeOfDayText(status.till),
			name(status.outside));
	case ScheduleStatusKind::OutsideWindow:
		return status.nextStart
			? u"Schedule: %1 until %2"_q.arg(
				name(status.outside),
				WindowStartText(status.nextStart, QDateTime::currentDateTime()))
			: u"Schedule: %1"_q.arg(name(status.outside));
	}
	return QString();
}

QString ScheduleDeviceText() {
	const auto &device = ThisDevice();
	return device.id.isEmpty()
		? u"This device reports no id, so rulesets naming one skip it."_q
		: u"(this device: %1)"_q.arg(DeviceLabelText(device.id));
}

} // namespace Purple

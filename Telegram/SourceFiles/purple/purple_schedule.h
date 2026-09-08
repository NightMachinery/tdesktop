/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_settings.h"

namespace Purple {

// Starts the clock that moves the active preset at the boundaries written in
// settings.toml. Idempotent: the runner is a singleton and the first call is
// what builds it.
void StartSchedule();

// The "not today" switch. Nothing in settings.toml turns it on - it is a
// runtime decision, so it lives in state.toml beside the active preset.
// Unpausing catches up with wherever the schedule has got to in the meantime.
[[nodiscard]] bool SchedulePaused();

// `until' is unix seconds, and zero - the default, and what an unpause always
// writes - means the pause lasts until it is lifted by hand. A deadline is
// checked by the tick rather than by a timer, so a pause that ran out while
// the app was closed has expired by the time anything reads it again.
void SetSchedulePaused(bool paused, int64 until = 0);

// Whether the file describes a schedule at all, so the UI can leave the pause
// switch out rather than offer a control over nothing.
[[nodiscard]] bool ScheduleConfigured();

// What the schedule is doing, in one line: "Schedule: work until 17:00, then
// home", "Schedule: home until Mon 09:00", "Schedule paused until ...". Empty
// when the file describes no schedule, so a caller can leave the row out.
//
// Every branch is a state the file can genuinely be in, and each says which one
// it is rather than falling back to a sentence that would be a lie in it - a
// schedule switched off in the file, or one whose every ruleset is for the
// phone, must not read as "home until 09:00".
//
// Here rather than in the box that first wanted it because there are two boxes
// now - the preset picker and the editor - and a clock line spelled twice is
// two answers to the same question waiting to disagree.
[[nodiscard]] QString ScheduleStatusText();

// Which device the line above is about. Worth saying the moment a file can hold
// rulesets: two machines reading the same settings.toml show different
// schedules, and without the id there is nothing on screen that says why. It is
// also where the id to type into a ruleset comes from.
[[nodiscard]] QString ScheduleDeviceText();

// The word for a preset in a sentence, rather than the TOML key underneath it:
// the tab name the preset chose, or its name capitalised. Normal is named
// outright because it is a bypass and has no table to carry a display name.
//
// It lives here because every sentence the schedule builds needs it and so does
// every screen that offers a preset to put in a rule.
[[nodiscard]] QString PresetDisplayName(
	const Settings &settings,
	const QString &name);

} // namespace Purple

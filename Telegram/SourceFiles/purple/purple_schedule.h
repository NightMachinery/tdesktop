/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

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

} // namespace Purple

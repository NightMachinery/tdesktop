/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_screentime.h"

class PeerData;

namespace Window {
class SessionController;
} // namespace Window

// Purple: the half of screen time that watches. Everything that turns what it
// writes into a number is in purple/purple_screentime.h, in the core, and is
// tested there; this file only knows which of the app's signals is which event.
//
// The whole of it is off until [screen_time] enabled_p says otherwise. Off
// means no file is opened, no timer runs and no hook does anything but return,
// because a log of what you looked at is not a thing to start keeping because
// a version number moved.
namespace Purple {

// screentime.log, beside settings.toml. Never leaves the machine: nothing here
// or anywhere else in the fork reads it for sending, and the plan says it
// never will. See docs/purple/work_mode.md.
[[nodiscard]] QString ScreenTimeLogPath();

// The app-wide half: foreground and background, the preset, the no-input
// watchdog, the flush timer and the daily prune. Idempotent, like the schedule
// runner, and called once from Application.
void StartScreenTime();

// The per-window half: which chat is in front. Called once per session
// controller, and each one reports for itself.
void WatchScreenTime(not_null<Window::SessionController*> controller);

// One composer action, named the way [screen_time] and the export name it:
// "typing", "send", "voice", "attach", "reply", "edit". `peer' may be null,
// which drops the action - an action outside a chat is not one the log has a
// session to hang on.
//
// Typing is throttled here rather than at the call site, to one event per
// `action_span': the field fires per keystroke and the core reads one Action
// as a span of activity, so a burst would otherwise write a hundred lines to
// describe the three seconds they already describe.
void NoteScreenTimeAction(PeerData *peer, const QString &action);

// Everything the log holds, oldest first, with whatever has not reached disk
// yet already in it - a screen opened a second after a send must show that
// send. Empty while the feature is off.
[[nodiscard]] std::vector<Event> ScreenTimeEvents();

// Today's snoozes of one budget, by its index in [[screen_time.budgets]], and
// the note that one has just been taken. Kept beside the log rather than in
// state.toml because the core owns state.toml's schema and a snooze count is
// this client's bookkeeping, not a fact about Work Mode.
[[nodiscard]] int ScreenTimeSnoozesUsed(int budgetIndex);
void NoteScreenTimeSnooze(int budgetIndex);

// When the last snooze of this budget runs out, in unix ms, or 0 if none is
// running. The cover asks, because a snooze is what puts it away.
[[nodiscard]] int64 ScreenTimeSnoozeUntil(int budgetIndex);

// Whether today's bulletin for a soft budget has already been said in this
// chat, and the note that it just has. One line per bulletin in
// screentime_notices, so the app restarting is not a reason to say it again.
[[nodiscard]] bool ScreenTimeNoticeShown(int budgetIndex, uint64 peerId);
void NoteScreenTimeNotice(int budgetIndex, uint64 peerId);

} // namespace Purple

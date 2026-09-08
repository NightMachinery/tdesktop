/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Ui {
class GenericBox;
} // namespace Ui

namespace Purple {

// The schedule, edited from the app rather than from the file. Everything here
// writes settings.toml through the same splice ops the phone uses, one key at a
// time, so a file full of comments comes back out of an edit still full of
// them - and so the file stays the thing that decides, with this box as one
// more way of writing it rather than a second source of truth.
//
// It is a box and not a settings section because a schedule is a small thing
// looked at rarely and changed in one sitting: rulesets, their rules, and the
// two switches that hold the whole thing off.
void ScheduleBox(not_null<Ui::GenericBox*> box);

// What to call this machine in [devices], so the id a ruleset names is legible
// on every device that reads the same file. It lives beside the schedule
// because that is the only reason device ids exist at all.
void DeviceLabelBox(not_null<Ui::GenericBox*> box);

} // namespace Purple

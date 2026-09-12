/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Ui {
class RpWidget;
} // namespace Ui

namespace Purple {

// Binds every hotkey settings.toml declares onto a window, the way
// Shortcuts::Listen does for tdesktop's own commands: the peek key from
// [peek], and one key per preset that wrote a `hotkey'.
//
// They are deliberately not Shortcuts::Commands. That table is owned by
// tdata/shortcuts-custom.json and by the shortcuts settings page, and these
// keys are owned by settings.toml, where the rest of Work Mode is configured.
// Two files claiming the same binding is exactly the situation the config split
// exists to avoid - so they stay out of that table and reload with the file
// they belong to.
void ListenHotkeys(not_null<Ui::RpWidget*> widget);

// Says what ended the last peek, once, if a lock did.
//
// Called when the machine comes back - the screen unlocked, or the passcode
// entered - because that is the first moment there is anybody to tell. A peek
// ended by a lock is ended while nobody is looking, and a chat list that has
// quietly gone back to hiding things with no word about why is the kind of
// thing that reads as a bug.
//
// The reason is cleared as it is said, so it is said once and not on every
// unlock afterwards.
void PeekEndedNotice();

// What is left of a peek, as a clock: "4:12". A countdown that moves every
// second wants a second hand, which is why this is not `Purple::FormatSpan' -
// that one rounds to whole minutes, which is right for a chart and wrong for a
// number the reader is watching tick.
[[nodiscard]] QString PeekRemainingText(int seconds);

} // namespace Purple

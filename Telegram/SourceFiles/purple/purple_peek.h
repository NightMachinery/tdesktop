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

// What is left of a peek, as a clock: "4:12". A countdown that moves every
// second wants a second hand, which is why this is not `Purple::FormatSpan' -
// that one rounds to whole minutes, which is right for a chart and wrong for a
// number the reader is watching tick.
[[nodiscard]] QString PeekRemainingText(int seconds);

} // namespace Purple

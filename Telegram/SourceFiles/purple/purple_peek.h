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

// Binds the peek hotkey onto a window, the way Shortcuts::Listen does for
// tdesktop's own commands.
//
// It is deliberately not a Shortcuts::Command. That table is owned by
// tdata/shortcuts-custom.json and by the shortcuts settings page, and this key
// is owned by settings.toml, where the rest of Work Mode is configured. Two
// files claiming the same binding is exactly the situation the config split
// exists to avoid - so peek stays out of that table and reloads with the file
// it belongs to.
void ListenPeekHotkey(not_null<Ui::RpWidget*> widget);

} // namespace Purple

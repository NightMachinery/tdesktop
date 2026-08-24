/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class GenericBox;
} // namespace Ui

namespace Purple {

// Choose the active preset. Until this existed the only way to switch was to
// edit active_preset in state.toml by hand, which is fine for a config file and
// hopeless for something switched several times a day.
//
// The session is only read to count what the active preset is actually holding
// back. That number is the difference between a box that describes presets and
// one that shows whether the chosen one is doing what was meant.
void PresetBox(
	not_null<Ui::GenericBox*> box,
	not_null<Main::Session*> session);

// What the main menu entry says: "Work Mode" under Normal, and the preset name
// once one is active, so the current mode is legible without opening anything.
[[nodiscard]] rpl::producer<QString> PresetMenuLabel();

} // namespace Purple

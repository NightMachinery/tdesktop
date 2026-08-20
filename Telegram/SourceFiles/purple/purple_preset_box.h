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

// Choose the active preset. Until this existed the only way to switch was to
// edit active_preset in state.toml by hand, which is fine for a config file and
// hopeless for something switched several times a day.
void PresetBox(not_null<Ui::GenericBox*> box);

// What the main menu entry says: "Work Mode" under Normal, and the preset name
// once one is active, so the current mode is legible without opening anything.
[[nodiscard]] rpl::producer<QString> PresetMenuLabel();

} // namespace Purple

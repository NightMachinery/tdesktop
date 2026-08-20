/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Purple {

// Acts on the OS focus flag: enters the preset [focus_sync] names when a focus
// mode comes on, and puts back what was there when it goes off. Idempotent.
void StartFocusSync();

// The flag itself, which this file only reads. Whatever notices that macOS has
// entered a focus mode writes it - and that is deliberately a different thing
// from what acts on it, because the policy below is worth proving once while
// the detection is an undocumented moving target Apple owns.
//
// SetFocusActive is for a detector living inside the app. The flag is also
// plain `focus_active` in state.toml, so a detector living outside it - a
// Hammerspoon watcher, a shortcut - needs nothing from this header.
[[nodiscard]] bool FocusActive();
void SetFocusActive(bool active);

} // namespace Purple

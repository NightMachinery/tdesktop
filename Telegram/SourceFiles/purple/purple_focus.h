/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Purple {

// Starts both halves: the watcher that notices macOS entering a focus mode, and
// the policy that enters the preset [focus_sync] names and puts back what was
// there afterwards. Idempotent.
void StartFocusSync();

// The flag between them. They are two things on purpose: the policy is worth
// proving once and does not change, while the detection reads a file Apple owns
// and can reshape in a point release. Keeping the seam here means the fragile
// half can be replaced without touching the proven one.
//
// It is also plain `focus_active` in state.toml, so a detector living outside
// the app needs nothing from this header - though it would then be a second
// writer to a file the app rewrites itself, which is a race a dedicated file
// would not have.
[[nodiscard]] bool FocusActive();
void SetFocusActive(bool active);

} // namespace Purple

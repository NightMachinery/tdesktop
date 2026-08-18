/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_settings.h"
#include "purple/purple_splice.h"
#include "purple/purple_state.h"

namespace Purple {

// Fork-local configuration, kept as hand-editable TOML outside Telegram's own
// encrypted settings blob. See docs/purple/config.md.
[[nodiscard]] QString ConfigDirectory();
[[nodiscard]] QString SettingsFilePath();
[[nodiscard]] QString StateFilePath();

// The Premium features Telegram Desktop gates on the client alone. Features
// the server enforces are deliberately not covered - see docs/purple/premium.md.
[[nodiscard]] bool LocalPremium();
[[nodiscard]] rpl::producer<bool> LocalPremiumValue();
void SetLocalPremium(bool value);

// What settings.toml said, last time it said anything we could use. A file that
// stops parsing leaves this at the last good value rather than reverting to
// defaults, so a typo mid-edit does not reshuffle the user's chat list.
[[nodiscard]] const Settings &ActiveSettings();

struct Problems {
	QString error;
	std::vector<QString> warnings;

	[[nodiscard]] bool empty() const {
		return error.isEmpty() && warnings.empty();
	}
};

// Everything the last load could not make sense of, for the UI banner.
[[nodiscard]] const Problems &SettingsProblems();

// Fires when either of the two above changes, including on hot reload.
[[nodiscard]] rpl::producer<> SettingsChanges();

// The only writes to settings.toml besides the Premium toggle. Both return
// false if the file could not be edited, having changed nothing.
bool AddToList(
	const QString &list,
	PeerIdValue id,
	const MemberTitle &title);
bool RemoveFromList(
	const QString &list,
	PeerIdValue id,
	const MemberTitle &title);

[[nodiscard]] const State &CurrentState();
[[nodiscard]] rpl::producer<> StateChanges();

// Mutates state.toml through a callback so the write and the notification
// cannot be forgotten. Writes nothing if the callback changes nothing.
void UpdateState(Fn<void(State&)> apply);

} // namespace Purple

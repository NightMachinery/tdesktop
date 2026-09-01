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

// The write every file in that directory goes through: QSaveFile puts a
// temporary alongside the target and renames over it, so a crash mid-write
// cannot leave a truncated config behind. Exposed because importing a
// settings.toml from elsewhere is a write to the same file with the same
// requirement, and a second implementation of it would be one to keep in step.
bool WriteConfigFile(const QString &path, const QString &text);

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

// The pinned order of one of a preset's extra views, which the app owns the
// same way it owns list membership: the file is where it lives, and dragging a
// row is an edit to the file. See SetViewPinned().
bool SetViewPins(
	const QString &preset,
	const QString &view,
	const std::vector<PeerIdValue> &ids,
	const MemberTitle &title);

// The same for the preset's own main view. Writing a non-empty order is what
// takes the main view off the account's pins and onto its own.
bool SetPresetPins(
	const QString &preset,
	const std::vector<PeerIdValue> &ids,
	const MemberTitle &title);

// Writes a new empty [lists.x] table. The list exists and does nothing until a
// preset names it, which is deliberate - there is no splice that edits a
// list_order, and guessing which preset wanted it would be worse than saying so.
bool CreateList(const QString &name, const QString &title);

[[nodiscard]] const State &CurrentState();
[[nodiscard]] rpl::producer<> StateChanges();

// Mutates state.toml through a callback so the write and the notification
// cannot be forgotten. Writes nothing if the callback changes nothing.
void UpdateState(Fn<void(State&)> apply);

} // namespace Purple

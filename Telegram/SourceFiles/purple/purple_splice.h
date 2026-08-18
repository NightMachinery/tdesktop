/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_settings.h"

// The only writes the app ever makes to settings.toml. The file is hand-owned
// and its comments are the point of it being TOML at all, so nothing here
// re-serialises the document: we locate the members array through toml++'s
// source regions and edit the raw lines, leaving every byte we are not
// responsible for exactly where the user put it. See docs/purple/config.md.
namespace Purple {

// Resolves a peer id to the display name written as that line's trailing
// comment. Names go stale, so the comment is regenerated every time its line is
// rewritten rather than being read back from the file.
using MemberTitle = Fn<QString(PeerIdValue)>;

struct SpliceResult {
	QString text;
	bool changed = false;

	// Non-empty means nothing was written and `text` is the input unchanged.
	QString error;

	[[nodiscard]] bool ok() const {
		return error.isEmpty();
	}
};

[[nodiscard]] SpliceResult AddListMember(
	const QString &text,
	const QString &path,
	const QString &list,
	PeerIdValue id,
	const MemberTitle &title);

[[nodiscard]] SpliceResult RemoveListMember(
	const QString &text,
	const QString &path,
	const QString &list,
	PeerIdValue id,
	const MemberTitle &title);

// Sets one boolean under one table, keeping the key, the spacing and any
// trailing comment exactly as the user wrote them. Adds the key, and the table,
// if either is missing. Used for the Premium toggle in Settings, which is the
// only scalar the app owns in an otherwise hand-written file.
[[nodiscard]] SpliceResult SetTableBool(
	const QString &text,
	const QString &path,
	const QString &table,
	const QString &key,
	bool value);

// Exposed for the tests: the ids a list holds, in file order.
[[nodiscard]] std::vector<PeerIdValue> ListMembers(
	const QString &text,
	const QString &path,
	const QString &list);

} // namespace Purple

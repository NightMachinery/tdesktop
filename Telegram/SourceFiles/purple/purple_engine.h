/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_settings.h"
#include "purple/purple_state.h"

// Turns a preset and the lists it inherits from into a flat table of "for this
// list, show and notify are these". Resolution happens once per config or
// preset change; nothing here may be called per repaint.
//
// Kept free of tdesktop dependencies for the same reason as the parser: every
// policy in the spec is a rule about data, and rules about data are far easier
// to prove outside a running app. See docs/purple/config.md.
namespace Purple {

// What a chat is, for the catch-all lists. The engine never sees a PeerData.
enum class ChatKind : uchar {
	Private,
	Group,
	Channel,
	Bot,
};

[[nodiscard]] ListKind CatchAllFor(ChatKind kind);

struct EffectiveList {
	QString list;
	bool show = true;
	bool notify = true;

	// Only meaningful for lists that can hold groups; resolved per list so a
	// preset can exempt one list from the mention gate without exempting all.
	bool groupsRequireMention = true;
};

struct Resolved {
	QString preset;

	// Normal is not "a preset with everything on" but a bypass: the engine is
	// skipped entirely, so it cannot drift as preset features are added.
	bool normal = false;

	// Priority order, first match wins.
	std::vector<EffectiveList> lists;
	std::vector<PresetFolder> folders;
	bool groupsRequireMention = true;

	[[nodiscard]] const EffectiveList *list(const QString &name) const;
};

// Nothing if the preset does not exist or its inheritance chain is broken. The
// caller is expected to fall back to the last good resolution rather than to
// defaults - defaulting would quietly unhide every chat the user hid.
[[nodiscard]] std::optional<Resolved> Resolve(
	const Settings &settings,
	const QString &preset);

// The list a chat belongs to under this resolution: the first in priority order
// holding it, or the catch-all for its kind. Never null for a resolution built
// from settings that went through ParseSettings, which guarantees the four
// catch-alls exist.
[[nodiscard]] const EffectiveList *MatchList(
	const Settings &settings,
	const Resolved &resolved,
	PeerIdValue id,
	ChatKind kind);

// What the chat list and the notification gate actually ask.
struct Visibility {
	bool show = true;
	bool notify = true;

	// The chat is a group that only appears once it has an unread mention.
	bool mentionGated = false;
};

[[nodiscard]] Visibility Visible(
	const Settings &settings,
	const Resolved &resolved,
	PeerIdValue id,
	ChatKind kind);

[[nodiscard]] ResolvedCache ToCache(const Resolved &resolved);
[[nodiscard]] std::optional<Resolved> FromCache(const ResolvedCache &cache);

} // namespace Purple

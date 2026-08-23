/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_settings.h"
#include "purple/purple_state.h"

class QDateTime;

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

// The folders a preset selection exempts from hiding - the ones that said
// `filtered = false'. Saying nothing leaves a folder filtered, like every
// folder the preset does not name at all.
[[nodiscard]] std::vector<QString> ExemptFolderNames(
	const std::optional<std::vector<PresetFolder>> &folders);

// The folders a preset silences - the ones that said `notify = false'.
[[nodiscard]] std::vector<QString> SilencedFolderNames(
	const std::optional<std::vector<PresetFolder>> &folders);

struct EffectiveList {
	QString list;
	bool show = true;
	bool notify = true;

	// Only meaningful for lists that can hold groups; resolved per list so a
	// preset can exempt one list from the mention gate without exempting all.
	// Off unless a preset asks for it: a preset that only hides bots must not
	// also empty the chat list of every group nobody has mentioned you in.
	bool groupsRequireMention = false;

	friend bool operator==(
		const EffectiveList &,
		const EffectiveList &) = default;
};

struct Resolved {
	QString preset;

	// Normal is not "a preset with everything on" but a bypass: the engine is
	// skipped entirely, so it cannot drift as preset features are added.
	bool normal = false;

	// Priority order, first match wins.
	std::vector<EffectiveList> lists;

	// Nothing means the preset says nothing about folders, so every folder
	// shows. An empty vector means it named none, which is a deliberate "hide
	// the folder strip" and not the same thing at all.
	std::optional<std::vector<PresetFolder>> folders;

	// Names from `folders' that said `filtered = false', lifted out so the
	// common case - nobody asked - is an empty vector to test rather than a
	// walk of the folder list per hidden chat.
	std::vector<QString> exemptFolders;

	// Names from `folders' that said `notify = false'. Same reason as above:
	// the common case is nobody asked, and that has to be one empty vector to
	// test rather than a folder walk per mute query.
	std::vector<QString> silencedFolders;

	// The preset-wide default every list starts from. Off when no preset in the
	// chain says otherwise - see EffectiveList.
	bool groupsRequireMention = false;

	// Whether hiding means "gone from the app" rather than "absent from this
	// preset's view of the chat list". Off by default: a hidden chat stays
	// reachable through the forward picker, search and recent chats, and only
	// the view leaves it out. See Preset::hideEverywhere.
	bool hideEverywhere = false;

	// A peek is running, so the preset's hiding is suspended - but not its
	// silencing. Set by the gate from state.toml rather than by Resolve(): a
	// peek is transient and expires on a clock, which is also why ToCache()
	// does not carry it. A cached resolution restored with a peek in it would
	// leave the chat list revealed with nothing left running to put it back.
	bool peeking = false;

	[[nodiscard]] const EffectiveList *list(const QString &name) const;

	// Used to decide whether a reload actually changed anything, so a state
	// write that only moved a peek deadline does not rebuild every chat list.
	friend bool operator==(const Resolved &, const Resolved &) = default;
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

// What the schedule wants active at this local time: the preset of the first
// rule covering the moment, or Normal when rules exist and none does.
//
// Nothing at all when the schedule is off or has no rules, which is a different
// answer from wanting Normal and has to be: otherwise an empty [schedule]
// section would quietly force Normal over every other way of choosing a preset.
[[nodiscard]] std::optional<QString> ScheduleTarget(
	const Schedule &schedule,
	const QDateTime &now);

} // namespace Purple

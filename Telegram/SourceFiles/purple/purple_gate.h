/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_engine.h"

class PeerData;

// The seam between the engine and the app. Everything below purple_engine.h is
// pure data and is tested standalone; this is the one file that knows what a
// PeerData is, so the rest of tdesktop asks its questions here rather than
// assembling engine arguments at each call site.
namespace Purple {

[[nodiscard]] ChatKind KindOf(not_null<const PeerData*> peer);
[[nodiscard]] PeerIdValue IdOf(not_null<const PeerData*> peer);

// The resolution the active preset produced, recomputed whenever settings.toml
// or state.toml changes. Normal until a preset is chosen, and Normal is a
// bypass rather than a permissive preset.
[[nodiscard]] const Resolved &ActiveResolved();
[[nodiscard]] rpl::producer<> ActiveChanges();

// False under Normal, which is the whole point: every gate in the app tests
// this first, so an unconfigured fork pays one bool load and behaves exactly
// like upstream. Call sites must not skip it - the engine answers "show
// everything" under Normal too, but only this is cheap enough for hot paths.
[[nodiscard]] bool Filtering();

// Both assume Filtering(). Under Normal they still answer correctly, but the
// caller has already paid for a peer classification it did not need.
[[nodiscard]] Visibility VisibleFor(not_null<const PeerData*> peer);
[[nodiscard]] const EffectiveList *ListFor(not_null<const PeerData*> peer);

// Whether a peek is running: the active preset's hiding is suspended - every
// chat shows, no group is mention-gated, every folder is back - while its
// silencing is left exactly where it was. See docs/purple/work_mode.md.
[[nodiscard]] bool Peeking();

// Whether the active preset takes the chats it hides out of the whole app
// rather than out of its own view of the chat list. False for every preset
// that does not ask, and false while peeking, which hides nothing at all.
//
// The difference is what the rest of the app can still reach: under the
// default a hidden chat is missing from the preset's view and from nowhere
// else, so forwarding to it, searching for it and the recent-chats row all
// still work. See docs/purple/work_mode.md.
[[nodiscard]] bool HideEverywhere();

struct PeekChange {
	bool peeking = false;

	// Normal hides nothing, so there was nothing to reveal and nothing was
	// written. Worth reporting rather than leaving a keypress look broken.
	bool refused = false;

	// How long the peek that just started will run, or zero for "until it is
	// turned off", which is what auto_off = "off" means.
	int seconds = 0;
};

// Starts a peek, or ends the one running.
PeekChange TogglePeek();

// Whether the active preset is what is silencing this chat. Distinct from
// "muted": the preset only ever adds a mute, so a chat can be both, and the UI
// has to say which one an Unmute would actually lift.
[[nodiscard]] bool SilencedByPreset(not_null<const PeerData*> peer);

// The chat folders the active preset shows, in the order it named them.
// Nothing means it said nothing about folders, so all of them show; an empty
// vector means it named none, which hides the folder strip outright.
[[nodiscard]] const std::optional<std::vector<PresetFolder>> &ShownFolders();

// Whether anything is restricting the folder list at all. Guards both the
// display of the folder strip and, more importantly, saving its order.
[[nodiscard]] bool FoldersRestricted();

// Folders the preset marked `filtered = false': their chats are exempt from
// its hiding. Empty for every preset that does not ask, which is what keeps
// the folder lookup in History::purpleHiddenFromChatList() free - it is only
// reached for a chat that would otherwise be hidden, and only when this is
// non-empty.
[[nodiscard]] const std::vector<QString> &ExemptFolders();

// Folders the preset marked `notify = false': their chats are silenced. Empty
// unless a preset asks, which is what keeps the folder walk out of every mute
// query that does not need it. Unaffected by peek, which reveals but does not
// un-silence.
[[nodiscard]] const std::vector<QString> &SilencedFolders();

} // namespace Purple

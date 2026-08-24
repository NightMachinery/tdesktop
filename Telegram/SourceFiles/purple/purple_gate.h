/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_engine.h"
#include "purple/purple_splice.h" // MemberTitle

class PeerData;

namespace Main {
class Session;
} // namespace Main

// The seam between the engine and the app. Everything below purple_engine.h is
// pure data and is tested standalone; this is the one file that knows what a
// PeerData is, so the rest of tdesktop asks its questions here rather than
// assembling engine arguments at each call site.
namespace Purple {

[[nodiscard]] ChatKind KindOf(not_null<const PeerData*> peer);
[[nodiscard]] PeerIdValue IdOf(not_null<const PeerData*> peer);

// The other direction, for the trailing comments the app writes beside the ids
// it splices into settings.toml. Names go stale, so a comment is regenerated
// every time its line is rewritten rather than read back from the file.
[[nodiscard]] MemberTitle TitleResolver(not_null<Main::Session*> session);

// The resolution the active preset produced, recomputed whenever settings.toml
// or state.toml changes. Normal until a preset is chosen, and Normal is a
// bypass rather than a permissive preset.
[[nodiscard]] const Resolved &ActiveResolved();
[[nodiscard]] rpl::producer<> ActiveChanges();

// What to write on the preset's main tab: its `default_view_name', or its name
// with the first letter capitalised. Never empty while a preset is running.
[[nodiscard]] QString ViewName();

// The extra tabs the active preset invents, in the order the file gave them and
// after its main view. Empty for every preset that does not ask, which is what
// keeps the whole feature out of the way of a preset that only hides chats.
[[nodiscard]] const std::vector<ResolvedView> &ExtraViews();

// Whether extra view `index' shows this chat. Membership only: an extra view
// never changes what a chat may do, so there is no notify half to ask about.
//
// Unaffected by peek, unlike the main view. A peek suspends *hiding*, and an
// extra view hides nothing - it is a selection the user asked for by name, and
// filling it with every chat during a peek would only take it away.
[[nodiscard]] bool ExtraViewHolds(int index, not_null<const PeerData*> peer);

// The ids extra view `index' pins, in the order settings.toml gave them. Empty
// for a view that has never had anything dragged in it, which is most of them.
[[nodiscard]] const std::vector<PeerIdValue> &ExtraViewPins(int index);

// Writes that order back. Unlike everything else about a view this is a fact
// the app discovers rather than reads - the user drags a row - so it is the one
// thing a view owns that has to travel in this direction.
bool SaveExtraViewPins(
	int index,
	const std::vector<PeerIdValue> &ids,
	const MemberTitle &title);

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

// The chat folders the active preset shows, in the order it named them,
// possibly including the "*ALL" marker that stands for every folder the entry
// did not name. Empty means no folder tabs at all, which is what a preset that
// says nothing about folders asks for.
[[nodiscard]] const std::vector<PresetFolder> &ShownFolders();

// Whether anything is restricting the folder list at all. Guards both the
// display of the folder strip and, more importantly, saving its order.
//
// False only for a preset whose whole folder selection is "*ALL" - every
// folder, in the account's own order, with default flags - because that is the
// one shape where a strip index still means the folder it meant before.
[[nodiscard]] bool FoldersRestricted();

// Folders the preset marked `include_in_main_view_p = true': their chats join
// the main view whatever the lists decided. Empty for every preset that does
// not ask, which is what keeps the folder lookup in
// History::purpleHiddenFromView() free - it is only reached for a chat that
// would otherwise be hidden, and only when this is non-empty.
[[nodiscard]] const std::vector<ExemptFolder> &ExemptFolders();

// Folders the preset marked `notify = false': their chats are silenced. Empty
// unless a preset asks, which is what keeps the folder walk out of every mute
// query that does not need it. Unaffected by peek, which reveals but does not
// un-silence.
[[nodiscard]] const std::vector<QString> &SilencedFolders();

// Folders that said `badge_p = false': no count on their own tab, and their
// chats left out of the app badge. Empty unless a preset asks, which is what
// keeps the folder walk out of every badge query that does not need it.
[[nodiscard]] const std::vector<QString> &QuietFolders();

} // namespace Purple

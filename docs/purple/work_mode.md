# Work Mode

Work Mode is the reason this fork exists. It lets a named *preset* decide, for
every chat, whether it appears in the chat list at all and whether it may
interrupt you - so "work" can mean four colleagues and one channel, and
"evening" can mean everything except work.

Configuration lives in `settings.toml`; see
[config.md](config.md) for the file, its two owners, live reload and what
happens when it is wrong.

Nothing here is active until a preset is chosen. The default is a preset named
`normal`, which is not "a preset with everything switched on" but a bypass: the
engine is skipped entirely. That matters as features accumulate - a bypass
cannot drift away from stock behaviour, a permissive preset can.

## The model

A **list** is a named set of chats with a `show` and a `notify` flag. Lists are
in priority order, top wins, so the first list holding a chat decides how it
behaves. Four **catch-alls** - `@private`, `@groups`, `@channels`, `@bots` -
always sort below your own lists and are always present, synthesized if the file
omits them. Every chat therefore matches exactly one list, and there is no
"unlisted chat" case anywhere in the engine.

A list may be **locked**, which means no preset can override it. That is what
makes an emergency list trustworthy: no amount of preset editing can silence it.

A **preset** is a set of overrides on those lists, plus an optional parent to
inherit from. Resolution walks the chain from the most derived preset upwards
and takes the first explicit value for each list, falling back to the list's own
default.

## What it does today

Two things, both derived from the resolved list a chat falls into:

- `show = false` removes the chat from every chat list.
- `notify = false` mutes it.

`show` is enforced in `History::shouldBeInChatList()`, ahead of every other
condition there - including the shortcut that keeps pinned dialogs, or pinning a
chat would exempt it from every preset.

That single hook also settles the unread badges, which was the part that looked
like it would need its own gate. `Dialogs::MainList` does not compute its totals
on demand; it accumulates them as entries enter and leave, through
`addEntry`/`removeEntry`. A chat that `shouldBeInChatList()` rejects is not in
any list, so it is in no total either - main list, archive and every chat filter
alike - and there is nothing left to subtract by hand.

`notify` is enforced in the private `Data::NotifySettings::isMuted(peer, ...)`,
which is the single root every mute question flows through: the notification
manager, the mute bell in the chat list, sorting, and `History::muted()` - which
is what splits unread counts into muted and unmuted for the badge. Hooking one
accessor gets all of them.

The preset can only ever *add* a mute. A chat you muted yourself stays muted
whichever list it lands in, so switching presets can never un-silence something
behind your back.

Saved Messages is never hidden and never gated. Nothing arrives in it unbidden,
and a chat list that does not show it offers no way back to it.

## Applying a preset change

Nothing about any peer changes when a preset does, so no upstream signal fires.
`Data::Session::refreshPurpleWorkMode()` walks every peer itself and for each
one re-evaluates the cached mute and then the chat list membership, in that
order. The order is load-bearing: hiding a chat takes its unread out of the
running totals, and it has to already be counted as muted or not when it goes,
or the totals drift by whatever that chat was carrying.

Membership takes two calls, `updateChatListExistence()` then
`updateChatListSortPosition()`, because neither does both directions. The first
drops a chat that is now hidden. Only the second brings one back: leaving the
chat list zeroes the sort key, and `setChatListExistence(true)` quietly removes
rather than adds when the entry has none.

The walk is triggered by `Purple::ActiveChanges()`, which fires only when the
resolution actually differs. A state write that merely moved a peek deadline
compares equal and rebuilds nothing.

It logs what it did:

    Purple: preset 'test', 6 lists.
    Purple: 100 of 2725 loaded chats hidden, 318 silenced.

That line exists because a preset that hides nothing looks exactly like a preset
that is working, and the usual cause is a list named slightly wrong. It is also
the only practical way to check the feature: an occluded macOS window is not
repainted, so screenshotting the chat list to see what changed returns the frame
from before the change.

The very first resolution, computed while `Data::Session` is still constructing,
fires before that session has finished subscribing, so nobody walks the peers
for it. That costs nothing: no peers exist yet at that point, and every chat is
filtered as it loads, through `shouldBeInChatList()`.

## When the active preset stops resolving

A preset can be deleted or renamed while it is active, or have its inheritance
chain broken mid-edit. The engine then runs on `resolved_cache` in `state.toml` -
the last resolution that worked - rather than falling back to defaults.

This is deliberate and worth stating plainly: defaulting would unhide every chat
you had hidden, which is the one outcome a work mode must never produce by
accident. If there is no cache either, the resolution already in effect stays in
effect and the reason is logged.

The cache carries `show` and `notify` per list, but not per-list mention gating -
one preset-wide value is enough to keep behaviour stable through a broken
reload. List *membership* is not cached: it comes from `settings.toml`, which
parsed successfully, since a file that did not parse leaves the previous
settings in place.

## Not yet implemented

- The mention gate. `Visibility::mentionGated` is resolved and available, but
  nothing acts on it, so a group configured to appear only on an unread mention
  currently just appears. That is the safe direction to be wrong in - nothing
  disappears unexpectedly.
- Folders, peek, scheduling and OS focus sync. All parsed, none consumed.
- The UI. There is no way to choose a preset from inside the app yet. Set
  `active_preset` in `state.toml` by hand - it is reloaded live, like
  `settings.toml`, so the chat list rearranges as you save.
- The mute bell and the chat context menu show a preset-imposed mute as if you
  had set it, and unmuting will not lift it. Making the UI say so is part of the
  read-only mirrors milestone.

## Cloud unread counts

`Dialogs::MainList` keeps a second total, `_cloudUnreadState`, from what the
server reports for chats that have not loaded yet. It is not gated, so a badge
can briefly count hidden chats between launch and the dialog list arriving. It
resolves itself as soon as the entries load.

## Implementation

    Telegram/SourceFiles/purple/purple_engine.{h,cpp}   resolution, pure data
    Telegram/SourceFiles/purple/purple_gate.{h,cpp}     the seam to tdesktop

`purple_engine` has no tdesktop dependency at all - it never sees a `PeerData`,
only an id and a `ChatKind` - for the same reason the parser does not: every
policy in the spec is a rule about data, and rules about data are far easier to
prove outside a running app. `purple/test_config.sh` compiles it standalone and
covers the resolution acceptance tests along with the config ones.

`purple_gate` is the single file that knows both sides. It classifies a
`PeerData` into a `ChatKind`, maps its `PeerId` to the plain numeric id the
config file uses, and holds the current resolution so that the answer to "is
this chat visible" is a table lookup rather than a walk of every list.

Call sites test `Purple::Filtering()` first. It is false under `normal`, which
is what keeps an unconfigured fork at one bool load per query rather than a peer
classification.

The fork's whole diff is findable with `git grep Purple::`.

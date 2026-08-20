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

Three things, all derived from the resolved list a chat falls into:

- `show = false` removes the chat from every chat list.
- `notify = false` mutes it.
- `groups_require_mention = true` leaves a group out until it holds an unread
  mention.

`show` and the mention gate are both enforced in
`History::shouldBeInChatList()`, ahead of every other condition there -
including the shortcut that keeps pinned dialogs, or pinning a chat would exempt
it from every preset.

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

## The mention gate

A preset may say `groups_require_mention = true`, and a list may override it.
A group in a gated list appears in the chat list only while it holds an unread
mention, and leaves again once that mention is read.

It is off unless a preset asks for it. That default was the other way round
until the gate was implemented - the value resolved but nothing consumed it, so
nothing felt it - and it is the worst default available: a preset written to
hide bots and nothing else would also have emptied the chat list of every group
nobody had mentioned you in.

Only groups are gated, and only ones that are showing at all. Channels have no
mentions in the relevant sense, and a hidden chat is hidden whether or not
anyone mentioned you in it - so `mentionGated` implies `show`.

The gate reads `chatListUnreadState().mentions`, which is the same number the
mention badge is drawn from. That is deliberate: the rule becomes legible from
the chat list itself - the group is there exactly while the badge would be lit.
It also gets forums right for free, since for a forum that number is the sum
over its topics rather than a count on the group.

Reading the mention is what takes the group away again, which means a group you
opened *from* its mention leaves the list while you are still standing in it.
The chat stays open; only the row goes. That is the feature working. The
alternative - exempting whichever chat is currently open - would mean the data
layer asking the window layer what is on screen, and would only postpone the
disappearance to the moment you look away.

Gating is orthogonal to `notify`. A gated group with `notify = true` still
announces every message, which mostly defeats the point; the combination worth
writing is `notify = false` with the gate on, which is "silent, and out of sight
until someone actually wants me". They are kept separate because a list decides
both, and collapsing them would remove a choice rather than add one.

### Re-checking it

Membership now depends on something that changes constantly, which the chat
list has no reason to re-examine on its own - `shouldBeInChatList()` is not
re-evaluated when an unread count moves.

The trigger is `HistoryUnreadThings::Proxy::setCount()`, which is the single
funnel every mention count change passes through, and which already computes
the has-any/has-none edge for the badge. The Purple hook sits just after that
existing dispatch, and outside its `inChatList()` guard, for two reasons: the
chat that needs bringing back is precisely the one that is not in the list, and
for a forum it is that dispatch which rolls the topic's count up into the
parent's sum.

It is guarded on the chat actually being gated. A mention edge cannot change
membership otherwise, and the refresh repaints a chat list row.

A transition is logged, by peer id:

    Purple: mention gate revealed peer 1234567890.

That is the one event which explains a chat appearing or vanishing with nobody
touching anything, so it is worth a line. It stays rare by construction - only
gated chats reach it, and only on the edge.

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
rather than adds when the entry has none. Both callers go through
`History::purpleRefreshChatListMembership()` so that ordering is stated once.

The walk is triggered by `Purple::ActiveChanges()`, which fires only when the
resolution actually differs. A state write that merely moved a peek deadline
compares equal and rebuilds nothing.

It logs what it did:

    Purple: preset 'test', 6 lists.
    Purple: 43 of 2251 loaded chats hidden, 203 mention-gated (2 showing), 0 silenced.

Gated groups are counted apart from hidden ones because it is a different
claim. A gated group is only out of the list while it has nothing to say, so
the number that means anything is how many of them are still showing - and a
zero gated count means the gate is off rather than that it found nothing.

That line exists because a preset that hides nothing looks exactly like a preset
that is working, and the usual cause is a list named slightly wrong. It is also
the only practical way to check the feature: an occluded macOS window is not
repainted, so screenshotting the chat list to see what changed returns the frame
from before the change.

The very first resolution, computed while `Data::Session` is still constructing,
fires before that session has finished subscribing, so nobody walks the peers
for it. That costs nothing: no peers exist yet at that point, and every chat is
filtered as it loads, through `shouldBeInChatList()`.

## Folders

A preset may also name which chat folders appear, in the order it names them:

    [presets.work]
    folders = [ { name = "Work" }, { name = "Uni" } ]

Saying nothing about folders leaves all of them showing. Saying `folders = []`
is different, and deliberate: it names none, which collapses the folder strip
altogether. That distinction has to survive resolution and the `state.toml`
cache, so both carry an optional rather than a plain list, and the cache writes
the key only when the preset said something.

Names are matched against folder titles, case-insensitively. A name matching no
folder is skipped and logged, for the same reason the hidden-chat count is
logged - a folder named slightly wrong is indistinguishable from a folder the
preset meant to hide.

"All chats" is never dropped. It is the only view that shows a chat belonging to
no folder, and the preset has already decided what that view contains. It is
also what makes `folders = []` work without a special case: left alone it takes
`ChatFilters::has()` below its threshold and the folder UI removes itself.

### Why a second accessor rather than filtering the real list

`ChatFilters::list()` is the account's actual folders, and far more than the
strip reads it - the folder settings page, the "add to folder" menu, the
Premium folder-count limit, the per-row folder tags, and the code that computes
which filters a chat belongs to. Restricting that one accessor would quietly
corrupt all of them.

So display surfaces read `purpleShownList()` and everything that edits or counts
keeps reading `list()`. When nothing is restricted the two are the same object,
not a copy, so Normal costs nothing.

The rule inside a display surface is all-or-nothing: those files convert between
a strip index and a filter constantly, and mixing the two lists would make
right-clicking the third tab act on the wrong folder.

### Reordering

Reordering is refused while a preset restricts folders, in two places.

`ChatFilters::saveOrder()` replaces the whole server-side order with exactly the
ids it is handed. Handed a subset, it would drop every hidden folder from the
account rather than from the view. That guard is the one that matters, because
it is a choke point: a display surface that was missed still cannot do damage
through it.

The two drag handlers also bail out early, so a drag is simply inert rather than
appearing to work and then snapping back.

To reorder folders, switch to `normal` first.

### notify is parsed but not implemented

`{ name = "Work", notify = false }` parses and warns. Silencing a whole folder
is not implemented, and the reason is not effort: a folder carrying Telegram's
"Exclude muted" flag stops containing a chat the moment it is silenced, which
un-silences it, which puts it back. That needs a decided rule rather than a
guessed one, and per-chat lists already cover the same ground.

`filtered` is likewise parsed and warned about.

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

## Choosing a preset

The main menu carries the switch, above Settings rather than inside it: a work
preset is changed several times a day and settings are not. The entry reads
`Work Mode` under Normal and `Work Mode: work` otherwise, so the current mode is
legible without opening anything.

The box lists Normal and every preset in the file, each with a one-line summary
of what it does - `work  -  hides 2 lists, silences 1 list, 3 folders` - built by
resolving the preset rather than by describing what was typed. Choosing one
applies it immediately and leaves the box open, so a wrong guess is one click
from being undone.

`state.toml` remains the source of truth and is still hand-editable; the box is
a second writer to the same field, not a replacement for it.

### What the box says when something is wrong

`settings.toml` problems - the error, and every warning - are shown here. Until
now they only reached the log, which meant a preset that silently did nothing
because of a mistyped list name looked exactly like a preset that was working.

If the active preset is not in the file at all - deleted, renamed, or lost to a
half-finished edit - no row is checked, and the box says so. Checking Normal
instead would look tidier and would be a disaster: the selection callback would
fire and switch the account to Normal, unhiding every chat the missing preset
was hiding, over a typo. This is the same rule as the `resolved_cache` fallback,
enforced in the UI rather than in the engine.

The rows are rebuilt when `settings.toml` changes, but deliberately *not* when
`state.toml` does. Choosing a preset is itself a state write, so rebuilding
there would destroy the radio button whose click was still on the stack. The
selection alone follows the state, which is what lets a schedule move it under
an open box.

## The mute a preset imposed

`Data::NotifySettings::isMuted()` answers the effective question, and a preset
can be what makes it true. The chat list context menu used to take that at face
value and offer `Unmute`, which lifted nothing - the next call went straight
back to muted.

It now says `Silenced by 'work'` and opens the preset box, which is the only
control that actually moves it. Below that, the ordinary mute item is chosen
from `purpleMutedWithoutPreset()` rather than from the effective answer, so
muting a chat yourself stays reachable while a preset silences it - and stays in
force once the preset stops.

That is the real accessor with the preset gate removed, not a peek at the
chat's `muteUntil`. The peek was the first attempt and it was wrong: a channel
can be muted by the account-wide default for its type with nothing set on the
chat at all, and it read those as unmuted. The split is one function boundary -
`isMuted()` is the gate plus the original body, and the body is what the UI
asks for separately.

The mute bell drawn on the chat list row is left alone. It is not a lie: the
chat really is silenced.

## Not yet implemented

- Peek, scheduling and OS focus sync. All parsed, none consumed.
- Per-folder `notify` and `filtered`, as above.
- Every other mute surface - the profile page toggle, the chat top bar - still
  shows a preset-imposed mute as the user's own. They are honest about the
  chat being silent and dishonest about who silenced it, and each needs the
  same two lines the context menu got.

## Cloud unread counts

`Dialogs::MainList` keeps a second total, `_cloudUnreadState`, from what the
server reports for chats that have not loaded yet. It is not gated, so a badge
can briefly count hidden chats between launch and the dialog list arriving. It
resolves itself as soon as the entries load.

## Implementation

    Telegram/SourceFiles/purple/purple_engine.{h,cpp}     resolution, pure data
    Telegram/SourceFiles/purple/purple_gate.{h,cpp}       the seam to tdesktop
    Telegram/SourceFiles/purple/purple_preset_box.{h,cpp} the preset picker

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

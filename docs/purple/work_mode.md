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

- `show = false` keeps the chat out of the preset's view of the chat list.
- `notify = false` mutes it.
- `groups_require_mention = true` leaves a group out until it holds an unread
  mention.

## The preset view

A running preset does not empty the chat list. It shows a **different one**.

The main list keeps every chat, exactly as it would under `normal`. Alongside
it the fork maintains one more chat list - the *preset view* - holding the
chats the preset does not hide, and the folder strip offers that view where
"All chats" would be. All chats itself is gone from the strip for as long as
the preset runs.

### What the tab is called

`default_view_name` on the preset, or - when it says nothing, which is the
usual case - the preset's own name with its first letter capitalised, so
`[presets.work]` gives a tab reading `Work`.

Only the first letter. A preset called `deep focus` becomes `Deep focus`, not
`Deep Focus`: guessing at word boundaries in a name someone chose is how you
end up mangling one, and a preset written `[presets.OS]` should not come back
shouted differently than it was typed.

It is the one preset field that is **not** inherited. Everything else walks the
chain and takes the first explicit value - `hide_everywhere`, the folder
selection, the list overrides - because those are policy, and a child that says
nothing means "same as my parent". A name is not policy. A child preset that
inherited its parent's label would put the parent's name over a different chat
list, which is the exact opposite of what a label is for, so a preset that does
not name its own view falls back to its own name rather than to its parent's.

The preset box and the `Work Mode: work` menu entry keep using the preset's
real name, because that is its identity - the string you type in `settings.toml`
and in a schedule rule. `default_view_name` is a label for one tab, not a
rename.

That is the whole design, and it is worth stating why it is not the obvious
one. The obvious one - the fork's first - was to make `shouldBeInChatList()`
answer no, so a hidden chat was in no list at all. It works, and everything
downstream comes free, but it fights tdesktop the whole way:

- **The invariant.** Being in a filter implies being in the main list;
  `Entry::notifyUnreadStateChange()` asserts it outright. Removing a chat from
  the main list therefore removes it from every folder too, whether or not that
  was wanted, and "hidden from All chats but present in its own folder" was not
  expressible at any price.
- **Pins.** `Entry::removeFromChatList()` unpins what it removes, which is
  right for a chat that has genuinely gone and wrong for one that is merely out
  of view. It cost a real pin loss on a real account before it was caught, and
  a latent `f_force` path that would have taken the pins off every device.
- **Everything else that reads the main list.** The forward picker, search
  suggestions, recent chats. A chat in no list is missing from all of them,
  which is a much larger claim than "keep my chat list quiet".

As a view, all three stop being problems rather than being solved one at a
time. The main list is complete, so the invariant holds by construction, the
pin logic never fires, and a hidden chat is still reachable everywhere the
preset is not looking.

The view is a filter, with the reserved id `Data::kPurpleViewFilterId`. Being a
filter rather than a special case is what makes the chat list machinery accept
it: `Dialogs::MainList` accumulates unread totals through `addEntry`/
`removeEntry` and sorts by `Row::sortKey(filterId)`, so the view's badge and
order are right for the same reason every folder's are. Membership is
maintained in `Session::refreshChatListEntry()`, next to the loop that does the
same for real folders.

Folders belong to the view exactly as they belong to All chats - the Archive
row is in it. Archived chats are not, and neither are forum topics or
Saved Messages sublists, which live in lists of their own.

### What the badge counts

`Session::purpleBadgeList()` returns the view while a preset runs and the main
list otherwise, and the seven badge accessors read it. This is the one place
the design costs something the removal-based one got free: the main list is now
complete, so counting it would leave the dock badge claiming unread messages
that nothing on screen accounts for.

They are the same object under `normal`, so nothing is paid there.

### Pins in the view

The view's pinned list is a **copy** of the main list's, minus the chats the
preset hides, retaken by `Session::refreshPurpleViewPinned()` whenever either
moves. Pinning is a fact about the account, not about the view, so the view is
never allowed to own pins of its own:

- Pinning or unpinning inside the view acts on the main list and sends the
  ordinary `messages.toggleDialogPin`.
- Dragging pins about inside the view reorders the main list. Two chats
  adjacent in the view may have a hidden chat between them in the main list;
  `PinnedList::reorder()` works on keys rather than indices, so the hidden one
  rides along instead of being swapped by mistake.
- The order saved to the server is the main list's, complete. The view is never
  sent anywhere - it is not a folder and no server has heard of it.

`Entry::removeFromChatList()` leaves the view's pinned list alone for the same
reason: the copy owns it, and the next copy would undo anything done here.

### hide_everywhere

    [presets.away]
    hide_everywhere = true

Global absence is a real thing to want from a work mode, so it is available -
but as a request rather than a side effect. `hide_everywhere = true` restores
the original behaviour: `History::shouldBeInChatList()` answers no, the chat
leaves the main list, and with it the forward picker, search suggestions and
recent chats.

It inherits down a preset chain like every other preset-wide switch, first
explicit value wins, and a child can turn its parent's back off.

Under it the exempt-folder rule below tightens back up, and for the original
reason: a chat out of the main list cannot be in a folder either.

### Hiding is a view, not an edit

A preset takes a chat out of what is on screen. It must never change anything
the account would still be carrying tomorrow, and there is one place that was
not true.

`Dialogs::Entry::removeFromChatList()` unpins whatever it removes. That is
right upstream, where an entry leaves because it has genuinely gone - you left
the group, it was deleted - and the server has unpinned it too. It was wrong
here: the chat was still on the account and came back the moment the preset
stopped, but the pin did not, because nothing was left to restore it from.

The quieter half was worse. That same pinned list is what
`ApiWrap::savePinnedOrder()` sends, and it sends it with `f_force`, so
reordering pins while a preset hid one would have dropped the hidden chat from
the account and from every other device. Exactly the hazard the folder
`saveOrder()` guard exists for, in a place nobody had looked.

The view design retires the whole hazard: nothing is removed from the main
list, so the unpin never fires. The guard survives for `hide_everywhere`, where
it does - `purpleHiddenFromChatList()` is what asks - and the reasoning above is
kept because that is the case it still covers.

The mute path was checked for the same shape and is clean.
`NotifySettings::purpleRefreshMute()` goes to `updateLocal()`, which never
issues a request.

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

"All chats" is dropped, and the preset view stands in its place - see
[The preset view](#the-preset-view). It is the only tab that shows a chat
belonging to no folder, so something has to be there, and the view is what the
preset means by "the chat list" anyway. It is also what makes `folders = []`
work without a special case: left as the only entry it takes the strip below
its "more than one tab" threshold and the strip hides itself.

The view is not a folder and does not pretend to be one. Right-clicking it
offers Mark as read and the Work Mode box, not Edit and Delete; it wears the
All chats icon; and every path that edits, deletes, reorders or saves a folder
tests `Data::IsPurpleView()` and refuses.

`ChatFilters::defaultId()` returns the view while a preset runs, so a new
window opens on it and closing the archive falls back to it rather than to the
complete list.

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

A preset that says nothing about folders still allows reordering. Its shown
list is the real one with the view standing exactly where All chats stood, so
every index still means the folder it did before.

To reorder folders while a preset restricts them, switch to `normal` first.

### Exempting a folder

    folders = [ { name = "Family", filtered = false } ]

`filtered = false` is an escape hatch: the chats in that folder ignore the
preset's hiding, and its mention gate with it, so they show in the preset view
alongside the lists it did not hide. A folder that opted out of the preset
deciding what is on screen opted out of all of it, not half.

Saying nothing leaves a folder filtered, which is what every folder the preset
does not name already is. Only an explicit `false` exempts.

Note what a *filtered* folder now means, because it changed with the view. The
preset decides the view; a folder decides its own tab. So a chat the preset
hides is missing from the view and still present inside its folder. That is the
strict reading, it is what "a folder shows what the folder says" implies, and
it was not affordable before the view existed.

If a folder's contents do not belong in a work mode, do not show the folder -
that is what naming folders is for. `filtered = false` is the opposite lever:
it pulls the folder's chats *into* the view.

Under `hide_everywhere` the old rule comes back, and must: a chat out of the
main list cannot be in a folder either, because tdesktop guarantees the reverse
throughout - `Entry::notifyUnreadStateChange()` asserts on it outright. So an
exempt folder's chats stay visible everywhere, and a hidden chat is gone from
its folder as well.

The lookup is free for everyone who does not use it. It runs only for a chat
the preset would otherwise hide, and only when some folder actually asked, so a
preset with no exemptions never walks the folder list at all.

### Silencing a folder

    folders = [ { name = "Noise", notify = false } ]

The chats in that folder are silenced, on the same terms as a silenced list:
the preset only ever adds a mute, so a chat muted by hand stays muted whichever
folder it is in.

A list can already do this when the folder is a hand-picked set - the same ids
with `notify = false` - and for those the list is the simpler tool. What a list
cannot do is track a folder defined by a *rule*: "all groups", "non-contacts",
"everything except these three". Membership there moves on its own as chats
arrive, and only the folder form follows it.

#### Breaking the loop

The obvious implementation eats itself. A folder carrying Telegram's "Exclude
muted" flag stops containing a chat the moment that chat is silenced, which
un-silences it, which puts it back, forever.

The rule that settles it: **membership is decided as though this preset
silenced nothing.** `ChatFilter::contains()` takes `ignorePresetMute`, which
swaps the cached effective mute for `purpleMutedWithoutPreset()`, so the input
to the decision cannot depend on its output. An exclude-muted folder therefore
holds exactly the chats it would hold with the preset switched off, and the
preset silences those.

#### What it costs, and one thing it does not do

`Purple::SilencedFolders()` is empty unless a preset names a folder with
`notify = false`, and that emptiness is checked first, so every mute query in
every other configuration is untouched. When it is non-empty the walk is over
the folder list, and `contains()` for a hand-picked folder is a set lookup.

One honest limitation. A chat *entering* a silenced rule-based folder - by
receiving the message that makes an unread-filtered folder include it - does
not immediately refresh the cached `History::muted()` that sorting and the
badge split read. Every live `isMuted()` answer is right from the moment it
changes; the cached one catches up on the next notify refresh. Making it
immediate needs a hook on filter-membership changes, the way the mention gate
hooks `setCount()`, and that is worth doing with a real trigger rather than by
re-walking every peer whenever any message arrives.

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

## Putting a chat in a list

Right-clicking a chat - in the chat list, or from its profile - offers
`Work Mode`, a submenu of every list you wrote in `settings.toml`, checked
where the chat is already a member. Clicking one adds or removes it. That is
the whole of it, and it exists because the alternative was hunting for a peer
id by hand: the ids in `members` are not shown anywhere in Telegram's UI, so
building a list used to mean turning on an experimental option, reading the
number off a profile, and typing it into the file.

The write goes through the same splice as everything else the app writes to
that file, so your comments, your ordering and your blank lines survive it, and
the line it adds carries the chat's name as a trailing comment. Names go stale,
so the comment is regenerated whenever its line is rewritten rather than read
back and trusted.

Four things the submenu deliberately does:

- **It leaves the catch-alls out.** `@private`, `@groups`, `@channels` and
  `@bots` match by chat type; there is no membership to toggle. Their defaults
  are set in the file and explained in the preset box.
- **It offers locked lists like any other.** `locked` stops a *preset*
  overriding what a list does. It says nothing about who is in it, and an
  Emergency list nobody can add to is not much of an emergency list.
- **It does not appear at all** unless you have written a list of your own, so
  an unconfigured fork's menus are exactly upstream's.
- **It names what is deciding the chat**, while a preset is running: a first
  line reading `In 'Essentials': shown` or `In '@bots': hidden`, which opens the
  preset box. That is the question that brings anyone to this menu, and the
  answer is as often a catch-all as one of your own lists.

That last line reports what actually happened to the chat, not what the list
asked for. The two come apart for a chat an exempt folder rescued - `@bots`
says hide, the folder says keep - and there the line reads
`In '@bots': silenced, shown by a folder`. Printing the list's verdict instead
would put the word `hidden` over a chat sitting in the list, which is worse
than saying nothing. A gated group reads `hidden until a mention` for the same
reason: that is where it stands right now.

Membership is global, not per preset. A chat is in Essentials or it is not;
what changes between presets is what Essentials *does*. The line above tells
you which list won, which matters because the first list in `list_order`
holding a chat is the one that decides it - adding a chat to a list that ranks
below one already holding it changes nothing, and the line is how you see that.

Writing the file reloads it, which re-resolves the preset and rebuilds the chat
lists, so a chat you have just hidden or revealed moves immediately. If the
write fails - the file is not writable, or the list is written as an inline
table the splice will not edit a line at a time - nothing changes on disk or in
memory and a toast says so, with the details in the log.

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

### The rule that settles every other surface

Controls answer for the setting they move; indicators answer for the truth.

The bell is an indicator, so it shows silence. Everything you can click to
change the mute is a control, and each of them now reads
`purpleMutedWithoutPreset()` - which is `isMuted()` with only the preset gate
removed, and therefore identical whenever no preset is silencing anything:

- the Mute/Unmute button on the profile top bar, through
  `Info::Profile::NotificationsEnabledValue()`,
- the mute menu's checked state, which is that same value,
- the MUTE/UNMUTE bar at the bottom of a channel.

Two of those were worse than mislabelled. `MuteMenu::ToggleMuteForever()` and
`HistoryWidget::toggleMuteUnmute()` both computed the value to write by
negating the effective answer, so while a preset silenced a chat they always
saw "already muted" and sent an *unmute* - clearing a mute the user had set, or
writing one for a chat they had never muted, and changing nothing on screen
either way because the preset still held. A control that quietly edits the
account while appearing inert is the worst of the three failures here.

The mute menu also carries `Silenced by 'work'`, the same line and the same
box as the chat list menu, because a menu offering Mute for a chat that is
already silent is correct and baffling at once.

The notification-exceptions list in Settings reads it too. That page lists the
exceptions the user has set, and a preset can silence a chat that has none at
all - "muted" there would send someone looking for an exception that is not
there.

## Peek

A preset hides chats, and sometimes you want one of them without ending the
preset. `Ctrl+Shift+P` suspends the hiding for two minutes: every chat is back
in the list, no group is waiting for a mention, and every folder is in the
strip. Press it again to end it early.

    [peek]
    hotkey   = "Ctrl+Shift+P"
    auto_off = "2m"

`auto_off = "off"` leaves it running until it is turned off by hand.

### It reveals; it does not un-silence

A peek does not touch `notify`. The two halves of a preset answer different
questions - hiding is about what you can find, silencing is about what may
interrupt you - and a peek is a deliberate look at the chat list. Unmuting for
it would deliver a burst of notifications for chats already on the screen and
then take the mute back before you had dealt with them.

It also keeps the rest of the UI honest through a peek: `Silenced by 'work'` in
the chat list menu still says the true thing, because it still is.

### It is not part of the resolution

`Resolve()` never sets `peeking`. The gate does, from `state.toml`, after the
`resolved_cache` snapshot has been taken - so a peek can never be persisted
into the fallback. A cached resolution restored with a peek in it would come
back revealed, with nothing left running to put it back.

Everything else follows for free. `peeking` is a field of `Resolved`, so
toggling it fires `Purple::ActiveChanges()` and the peer walk in
`Data::Session::refreshPurpleWorkMode()` rebuilds every chat list exactly as it
does for a preset change.

### The flag and the clock

`peek_active` and `peek_deadline_unix` live in `state.toml`. The deadline is
local wall-clock seconds rather than the server clock: a two-minute peek has to
expire while offline too.

Persisting them is what would otherwise let a peek outlive an app that was
killed in the middle of one, and the deadline is what makes that safe - by the
time anything reads the flag again it has passed, so the peek is already over.
A peek with `auto_off` turned off has no deadline, and that one does come back,
which is what "until you turn it off" has to mean if it means anything.

The gate clears a flag it finds expired rather than leaving `state.toml`
claiming a peek that ended, because that flag is what the next press reads to
decide which way to toggle.

The timer belongs to the gate for the same reason: the gate is what must re-run
when it fires. Nothing else would look at the deadline again, so without it a
peek would sit there until the next unrelated config change.

### Why the key is not a Shortcuts::Command

tdesktop's shortcut table is owned by `tdata/shortcuts-custom.json` and by the
shortcuts settings page. This key is owned by `settings.toml`, where the rest of
Work Mode is configured, and two files claiming one binding is the situation the
config split exists to avoid. So `purple_peek.cpp` keeps one `QAction` and adds
it to every window - exactly what `Shortcuts::Manager` does - and re-points it
when the file changes, so the hotkey reloads live like everything else in there.

The sequence is read as Qt portable text, which means that on macOS `Ctrl` is
Command and `Meta` is the physical Control key. That is the same convention
tdesktop writes into its own shortcuts file.

### Finding it

The main menu entry reads `Work Mode: work (peeking)` while one is running, and
the preset box carries a checkbox naming the key: a hotkey with no visible
affordance is a hotkey nobody remembers. Under Normal the checkbox is inert and
says why - `Peek - nothing is hidden under Normal` - rather than sitting there
greyed out with no explanation.

Pressing the key shows a toast saying which way it went - including under
Normal, where the answer is that there was nothing to do.

## The schedule

    [[schedule.rules]]
    days   = ["mon", "tue", "wed", "thu", "fri"]
    from   = "09:00"
    to     = "17:00"
    preset = "work"

Rules are matched in file order, first match wins - the same rule the lists
follow. Two rules covering one moment is something a hand-written file will do,
and picking by position is the only answer that can be predicted by reading.

Windows are half-open: `09:00` is inside one and `17:00` is not, so
neighbouring windows hand over cleanly instead of both claiming the minute they
meet.

A window whose `to` is earlier than its `from` crosses midnight, and belongs to
the day it starts on. `days = ["mon"]` with `22:00` to `06:00` runs from Monday
evening into Tuesday morning - not until midnight, and not also over Monday's
own small hours, which is what listing Tuesday as well would have produced.

### It acts at boundaries, not on every tick

The schedule compares what it wants against `schedule_target` in `state.toml`
and does something only when that changes.

That is what lets a preset chosen by hand stand. Pick `normal` at ten in the
morning inside a nine-to-five window and it stays, because the schedule's answer
has not moved since nine. Applying the answer on every tick instead would put
`work` back a second later and make the picker useless during exactly the hours
it matters.

It is also what makes a boundary missed with the app closed still happen. Open
the app at noon and the target has moved from whatever was recorded to `work`,
so it applies - once.

### What a boundary does, and what it leaves alone

A window starting overrides a preset chosen by hand. It is a positive
instruction, written down in advance: at nine, work mode.

A window ending does not. Its end only means the reason for that preset has
passed, which is no reason to undo something asked for, so Normal is applied
only when the preset in force is one the schedule itself put there. The
asymmetry is the point, and it is why `state.toml` records what put the current
preset in place rather than only what it is.

Focus is left alone in both directions. It is the more immediate signal, and a
schedule fighting it would leave neither of them predictable.

### Pausing

`schedule_paused` holds it off entirely, and the preset box carries the switch.
Nothing in `settings.toml` turns it on, because it is a decision about today
rather than about the configuration. The row is there only when the file
describes a schedule at all - a switch that holds off nothing explains nothing.

Unpausing catches up with wherever the schedule has got to, by the same boundary
rule: the target moved while it was not looking.

### The tick

Thirty seconds, which is therefore how late a boundary can be. Computing the
exact moment of the next one and sleeping until it would be tidier, and would
then have to survive every way a wall clock can move underneath it - a laptop
waking, a timezone change, the DST hour. Re-reading the clock on a cheap tick
survives all of them by construction.

A settings or state change re-ticks immediately, since either can change the
answer sooner than the next thirty seconds would.

## OS focus sync

    [focus_sync]
    enabled      = true
    enter_preset = "work"
    exit_preset  = "previous"

When the OS says a focus mode is on, the named preset takes over. When it goes
off, `previous` puts back both the preset and the reason it was active - so a
window the schedule had opened still closes at its own boundary afterwards. A
preset named outright instead was put there by neither the user nor the
schedule, so it lands as a manual choice and stays until something moves it.

### The flag and the detector are separate

`focus_active` in `state.toml` is the entire input. Everything above reads it,
and one thing writes it.

The split is deliberate, because the two halves age differently. The policy -
what to enter, what to put back, what to leave alone - is worth getting right
once and then does not change. Detecting a macOS focus mode is the opposite:
there is no public API for it, so the detector reads
`~/Library/DoNotDisturb/DB/Assertions.json`, which is undocumented, owned by
Apple, and free to change shape in a point release.

Keeping them apart means the fragile half can be rewritten, or replaced by
something outside the app, without touching anything already proven.

### Reading the focus state

A focus mode that is on is an assertion held in `data[0].storeAssertionRecords`.
The key is simply absent while nothing holds one, and a mode that has ended
moves to the invalidation records beside it - so a non-empty array is a focus
mode running now, and there is no need to interpret timestamps.

Every failure to make sense of the file leaves `focus_active` exactly as it was
and logs, once per spell rather than once per read. The alternative - reading a
parse error as "focus is off" - would end a session that is still running, which
is the one wrong answer that acts.

The watch is on the directory rather than the file, because the file is replaced
rather than rewritten and a watch on it would end up pointing at an inode nobody
will write to again. That is the same trap `settings.toml` has. A sixty-second
poll sits underneath as a backstop: a watch that quietly stopped working would
take focus sync with it and nothing would say so.

The path is macOS-only and empty everywhere else, where the detector does
nothing at all and leaves the flag to whatever else wants to set it.

### Edges again

`focus_seen` records the last value acted on - the same shape as
`schedule_target`, for the same reason. A preset chosen by hand in the middle of
a focus session stands, because nothing fires again until focus itself changes;
and when focus does end, the preset in force is not the one focus imposed, so it
is left alone.

Turning `enabled` off while focus is holding a preset hands that preset back. A
preset that nothing on screen explains and nothing still running would ever lift
is the one state this must not be able to reach.

## Not yet implemented

- A chat entering a silenced rule-based folder does not refresh the cached
  mute immediately, as above.

## Cloud unread counts

`Dialogs::MainList` keeps a second total, `_cloudUnreadState`, from what the
server reports for chats that have not loaded yet. It is not gated, so a badge
can briefly count hidden chats between launch and the dialog list arriving. It
resolves itself as soon as the entries load.

## Implementation

    Telegram/SourceFiles/purple/purple_engine.{h,cpp}     resolution, pure data
    Telegram/SourceFiles/purple/purple_focus.{h,cpp}      OS focus sync
    Telegram/SourceFiles/purple/purple_gate.{h,cpp}       the seam to tdesktop
    Telegram/SourceFiles/purple/purple_list_menu.{h,cpp}  list membership menu
    Telegram/SourceFiles/purple/purple_peek.{h,cpp}       the peek hotkey
    Telegram/SourceFiles/purple/purple_preset_box.{h,cpp} the preset picker
    Telegram/SourceFiles/purple/purple_schedule.{h,cpp}   the schedule clock

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

The preset view lives outside `purple/`, because it is a chat list rather than
a policy:

    Telegram/SourceFiles/data/data_chat_filters.h     kPurpleViewFilterId
    Telegram/SourceFiles/data/data_chat_filters.cpp   purpleViewFilter, defaultId
    Telegram/SourceFiles/data/data_session.cpp        membership, pins, badges
    Telegram/SourceFiles/history/history.cpp          purpleHiddenFromView

The fork's whole diff is findable with `git grep Purple::`.

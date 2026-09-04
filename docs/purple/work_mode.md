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

Two ideas, and that is all of it.

A **list** says who is in it. `members` names peer ids; `kinds` names chat types
- `private`, `groups`, `channels`, `bots` - and a chat matches when either half
does. A list carries no behaviour at all: nothing in `[lists.x]` says what
happens to those chats.

A **preset** says what happens. It writes an ordered `list_order`, and each
entry names a list and what that list means *for this preset*:

    [presets.work]
    list_order = [
      { list = "essentials", show_mode = "always", notify_p = true },
      { list = "bots",       show_mode = "never",  notify_p = false },
    ]

Order is priority **and** capture. Walking the order, the first entry whose list
holds a chat decides that chat, and nothing further down ever sees it again. A
chat no entry claims is **hidden and silenced**: a preset names what gets
through, and saying nothing about a chat is saying no - to both halves, because
a chat you are not looking at has no business interrupting you either.

### Why the model was rebuilt

It used to be two mechanisms doing one job. A list carried global `show` and
`notify` defaults, every preset then overrode them through a separate
`[presets.x.overrides.<list>]` table, a single top-level `list_order` fixed
priority for every preset at once, and `locked` existed only to stop overrides
reaching a list. Answering "what does this preset do to this chat" meant reading
four places at once and knowing which won.

Folding the flags into the ordered entry collapses all of it. Priority,
inclusion and behaviour become one statement in one place. `overrides` has
nothing left to do; `locked` has nothing to protect against, because a preset
can only reach a list it names.

The four catch-alls went with it. `@private`, `@groups`, `@channels` and `@bots`
were a special case threaded through the parser, the sort, the engine and the
menu - and a list with `kinds = ["bots"]` is the same thing without any of it.
`ListKind`, `IsCatchAll`, the forced-to-bottom sort and the synthesize-if-absent
pass are all gone. So is the "every chat matches exactly one list" invariant
they existed to provide: matching nothing is now a real answer, and it is the
one that means hidden.

### Reuse without inheritance

Presets used to inherit. `list_order` cannot be merged sensibly - two ordered
capture sequences do not compose - so a child would have had to restate its
whole order anyway, leaving inheritance to save only the odd flag. Against that
it cost a chain walk, loop detection, a reserved `default` name and an implicit
root preset.

It is replaced by spread. A bare `"*name"` string inside a `list_order` or
`folders` array splices in `[list_sets.name]` or `[folder_sets.name]`, the way
Python spreads a list:

    [list_sets.always]
    list_order = [
      { list = "os",        show_mode = "always", notify_p = true },
      { list = "emergency", show_mode = "always", notify_p = true },
    ]

    [presets.work]
    list_order = [ "*always", { list = "bots", show_mode = "never" } ]

Sets may refer to sets. A name mentioned twice keeps its first mention, which is
forced for a `list_order` - order *is* capture - and folders follow the same
rule so there is one to remember rather than two. Writing an entry and *then*
spreading a set that also holds it is the idiom this exists for: override one
thing, take the defaults for the rest. That case is silent. An explicit
duplicate still warns.

## What it does today

Two things, both derived from the entry a chat falls into - or from falling
through, which is a decision too:

- `show_mode` decides **when** the chat is in the preset's view of the chat
  list. Five values, and the ones in the middle depend on unread state, so a
  chat can come and go on its own:
  - `"always"` - it is simply there.
  - `"message"` - only while it has an unread message. A mention or a reply is
    a message; a reaction on its own is not. A manual unread mark counts.
  - `"message_or_reaction"` - as above, and a reaction alone is enough.
  - `"mention"` - only while it holds an unread mention. The narrow one.
  - `"never"` - out of the view.
- `notify_p = false` mutes it.
- no entry at all is `"never"` and muted, both.

**The default depends on what the chat is**, which is the part worth
remembering:

- a **channel** or a **bot** defaults to `"always"` - things you subscribed to
  or started, which you would not have if you did not want them;
- a **group** defaults to `"mention"` - the noisy ones speak up when they name
  you;
- a **private chat** defaults to `"message"` - it appears when the person has
  said something.

That is why `show_mode` cannot be collapsed when a preset resolves: one
`list_order` entry can claim private chats, groups and channels at once, so
`EffectiveList::show` stays unset and `Visible()` finishes the job with the
chat's `ChatKind` in hand.

Every *boolean* key in `settings.toml` ends in `_p`. `show_mode` and
`include_in_main_view` do not, because they are not yes-or-no - which is also
the visible sign that the two retired keys, `show_p` and
`groups_require_mention_p`, were the wrong shape for the question.

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
end up mangling one.

And only when the name has no capital in it anywhere. `[presets.OS]` must not
come back shouted differently than it was typed, and `[presets.iH]` must not
come back as `IH`. A capital anywhere means the casing was decided; the rule is
there to tidy a name nobody thought about, not to overrule one somebody did.

There is nowhere else it could come from. When presets inherited, this was the
one field deliberately exempted - a child taking its parent's label would have
put the parent's name over a different chat list, which is the exact opposite of
what a label is for. Inheritance is gone, so the exemption is now just how names
work, but the rule it encoded is worth keeping in mind if inheritance is ever
missed: a name is not policy, and spreading a set into a preset does not spread
a name into it either.

That one name is then used everywhere a preset is shown to a person: the tab,
the rows of the preset picker, the `Work Mode: Work` menu entry, the
`Silenced by 'Work'` line on a chat the preset muted, and the
`In no list 'Work' names` line at the top of the chat menu. `PresetTitle()` in
`purple_settings.cpp` is the single answer, and `Purple::ViewName()` is how the
four sites that mean *the active preset* ask for it.

They were split for a while, and it was worse than it sounds: the picker
offered `work` directly above a tab reading `Work`, and a preset that had
written a `default_view_name` had one name in the picker and a different one on
the tab it produced, with nothing anywhere saying they were the same preset.

The raw key survives in exactly one place - the `LOG()` lines - because a log
line is about the file rather than about the chat list, and the thing you would
go on to grep for is `[presets.work]`.

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

That id is the first of a run of `Data::kPurpleViewLimit` reserved ids - the
preset's main view, then one per extra view it declares. `Data::IsPurpleView()`
is a range test and `Data::PurpleViewIndex()` says which one, so the twenty-odd
places that only need "is this a tab the server never sent" stay a boolean and
the handful that need the identity ask for it. The cap lives here rather than in
the parser because it is a fact about filter ids, which a config file has no
business knowing about; a preset naming more views than that gets the first
fifteen and a log line saying so, because silently drawing fifteen would read as
"the rest are empty".

Folders belong to the view exactly as they belong to All chats - the Archive
row is in it. Archived chats are not, and neither are forum topics or
Saved Messages sublists, which live in lists of their own.

### The Archive row is hidden by default

Upstream leaves "Archived chats" sitting at the top of the chat list; this fork
defaults it to the main menu instead, which is where right-clicking it and
choosing "Move to main menu" puts it. The preset view honours the setting
exactly as All chats does, so the row is in neither or in both.

It is a plain default and nothing more, which means it only reaches a fresh
`tdata`. The setting lives in tdesktop's own per-account blob, which is
serialised whether or not anyone ever chose anything, so an account that has run
before already has a value there and keeps it. Right-click the row to move it
yourself on an existing account.

### What the badge counts

`Session::purpleBadgeList()` returns the view while a preset runs and the main
list otherwise, and the seven badge accessors read it. This is the one place
the design costs something the removal-based one got free: the main list is now
complete, so counting it would leave the dock badge claiming unread messages
that nothing on screen accounts for.

They are the same object under `normal`, so nothing is paid there.

### Pins in the main view

The main view's pinned list is a **copy** of the main list's, minus the chats the
preset hides, retaken by `Session::refreshPurpleViewPinned()` whenever either
moves. Pinning is a fact about the account, not about the main view, so it is
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
reason: the copy owns it, and the next copy would undo anything done here. That
holds for an extra view too, where the seed below owns it instead.

### Extra views

A preset may invent more tabs than its own:

    [[presets.work.views]]
    name   = "P0"
    pinned = [ 1234567890 ]
    list_order = [ { list = "close_people" } ]

Each becomes a tab of its own on the strip, after the preset's main view and
before any folder - a preset's tabs belong next to each other rather than
scattered through the account's folders. Each carries its own unread badge, for
the same reason the main view does: it is a `Dialogs::MainList` like any other,
so its total is accumulated by the same `addEntry`/`removeEntry` path.

A view's `list_order` selects membership and nothing else. `show_mode = "never"`
on an entry drops the chats that entry claims from this tab; falling through
drops them too, the same rule the main view follows. `notify_p` is meaningless
here and warns: a chat has one mute state however many tabs are showing it, and
silence belongs to the chat rather than to where you happen to be looking at it.

The unread-watching modes are deliberately **not** honoured on a view, and
neither are the per-kind defaults. A view is a selection you asked for by name,
and a "P0" tab that emptied itself whenever its chats went quiet would be the
opposite of the point. Only `"never"` removes; everything else, including
saying nothing, means "on this tab".

That asymmetry is what makes the useful pattern possible, and it is worth
naming, because it is not obvious from either half on its own. A list can be
**gated in the main view and unconditional on a tab of its own**:

    [list_sets.core]
    list_order = [
      { list = "close_people", show_mode = "message_or_reaction" },
    ]

    [[presets.work.views]]
    name       = "P0"
    list_order = [ { list = "close_people" } ]

The same three people are out of the Work tab until they have actually said
something, and permanently one click to the right of it. Nothing coordinates
the two: the main view asks `Visible()`, which collapses the mode, and the tab
asks `ViewHolds()`, which ignores it. Wanting quiet in the chat list and wanting
the people reachable are different wants, and they were being traded off against
each other only because there was one place to express both.

A view may show a chat the preset hides from its main view - a "later" tab for
what you are not looking at is a reasonable thing to want. The one exception is
`hide_everywhere_p`, where the chat is gone from the main chat list entirely and
tdesktop's "in a filter implies in the main list" invariant makes the pairing an
assertion failure rather than a preference. The parser refuses it with a warning
rather than leaving it to crash.

Three things an extra view deliberately does not do:

- **It holds no folders.** The Archive row is on the main view, as it is on All
  chats; an extra view's membership comes from lists of peers, and there is
  nothing a list could say that would put the archive on one. Its badge is
  counted accordingly: `Data::UnreadStateValue()` takes the archive's own total
  out of the main view, which would otherwise count it twice, and must not do
  that for an extra view - subtracting a total that was never added drove the
  first working build's tabs to `-334` and `-314`.
- **A peek does not touch it.** Peek suspends *hiding*, and an extra view hides
  nothing - it is a selection asked for by name, and filling it with every chat
  for two minutes would only take it away.
Saved Messages used to be a fourth item here, exempted from every tab the way
it was exempted from everything else. It is not any more; see below.

### Pins in an extra view

An extra view **owns** its pinned order. It is not standing in for All chats, so
there is no main-list order to copy, and nothing on the server has heard of the
tab. The order lives in the file:

    [[presets.work.views]]
    name   = "Focus"
    pinned = [
      1234567890, # Some Person
    ]
    list_order = [ { list = "essentials" } ]

`Session::refreshPurpleViewPins()` seeds the tab's `Dialogs::PinnedList` from
that array whenever the config or the membership changes, and pinning or
dragging inside the tab writes it back through `Purple::SetViewPinned()` - the
same splice that maintains `[lists.x] members`, so the rest of the file keeps
its bytes. Pinning inside an extra view does **not** pin on the account.

Four consequences worth stating, because each is a thing that could have gone
the other way:

- **The array is addressed by the view's name, not its position.** The file may
  hold `[[presets.x.views]]` blocks the parser dropped - unnamed, duplicated,
  naming no list - so the nth block in the file is not the nth tab on the strip.
  A pin written by index would land on a different view than the one dragged.
- **An id the tab does not currently hold keeps its place.** The usual reason a
  pinned chat is missing from a tab is that it has not finished loading, and
  dropping it would silently unpin it the first time anything else moved.
  `Session::savePurpleViewPins()` splices such ids back at the index the file
  gave them, so an unchanged order rewrites the file to the same bytes and the
  splice reports nothing changed.
- **The write is deferred to the next main-loop turn.** It comes back as a
  config reload, which rebuilds every chat list; doing that from inside the drag
  handler that asked for it would pull the rows out from under it.
- **The limit is the ordinary chat pin limit.** Nothing on the server bounds a
  local file, but a tab that is all pins is not a tab, so
  `Session::pinnedCanPin()` applies the same allowance.

A view whose ids no longer resolve to anything sorts by date, which is what an
empty `pinned` means anyway.

### hide_everywhere

    [presets.away]
    hide_everywhere_p = true

Global absence is a real thing to want from a work mode, so it is available -
but as a request rather than a side effect. `hide_everywhere_p = true` restores
the original behaviour: `History::shouldBeInChatList()` answers no, the chat
leaves the main list, and with it the forward picker, search suggestions and
recent chats.

Under it the folder rule below tightens back up, and for the original reason: a
chat out of the main list cannot be in a folder either. For the same reason a
preset that sets it may not also declare extra views - see below - because a
view showing a chat that is not in the main list is an assertion failure rather
than a preference. The parser refuses that pairing with a warning.

### How far a "hide until" reaches

    [overrides]
    hide_scope = "keep_in_folder_but_exclude_from_badge_count"

A "hide until" always takes the chat out of the preset's view; that much is what
the menu entry says. The folder tabs are a different question, because there the
chat is a member of a list the preset does not own, and hiding it there is
overruling a folder you built rather than applying a preset you chose.

Three answers, and the middle one is the default:

- `keep_in_folder_but_exclude_from_badge_count` takes the chat's unread out of
  every running total while the hide lasts. The row stays on its tabs and keeps
  its own badge - that is a fact about the chat - but the tab stops adding it
  up. This is the default because a badge is the part that pulls your eye back:
  a chat you put away half an hour ago should not be the reason a folder is lit.
- `hide_everywhere` reuses `purpleHiddenFromChatList()`, so one chat gets for a
  while what `hide_everywhere_p` gives a whole preset. It inherits that switch's
  honest hole too: a chat out of the chat list has no unread bookkeeping, so an
  unread-gated chat cannot come back on its own until the hide expires.
- `keep_in_folder` is what a hide did before the key existed.

Global rather than per-preset: the menu offers the same decision whichever
preset is running, and a reach that changed under you when a schedule swapped
presets would be a worse surprise than a key you have to set twice.

A peek suspends all three, and so does the `[recent]` close buffer, because both
are already answered by `History::purpleHiddenFromView()` - which
`purpleHideUntilReaches()` asks last, after the two cheap tests. A hide that is
not hiding anything at this moment has nothing to take out of a count. The order
of those tests matters: `shouldBeInChatList()` asks this of every row on every
refresh, so under a scope the file did not name it has to come to nothing on a
single enum comparison, without reaching the override list or the clock.

**The exclusion is cached, and that is the whole design.** Every list holding a
chat keeps a running unread total, moved by `notifyUnreadStateChange()` from the
old value to the new one. A predicate answered live would change both sides of
that subtraction at the same instant - the preset moves, and now the "before"
value is also the "after" value - and leave every total wrong until a restart,
silently. So `History::_purpleUncounted` is a flag, flipped only inside
`unreadStateChangeNotifier()`, which is the thing that pays the difference
across. `Session::refreshPurpleWorkMode()` flips it for every loaded chat
whenever the preset, the overrides or the peek move, in that order: mute first,
then the count, then the membership, each one settled before the next can move
the chat between lists.

It is set in the `History` constructor too, from the override alone. A chat that
loads while a hide is running would otherwise count towards its folders until
the next preset change, and at construction there is nothing to notify - the
chat is in no list yet, so the first list it joins simply reads the right
answer.

`chatListUnreadState()` carries `known` across rather than resetting it:
`Dialogs::MainList` asserts that a total which was known stays known, and it is
still known. It is zero.

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
list, so the unpin never fires. The guard survives for `hide_everywhere_p`, where
it does - `purpleHiddenFromChatList()` is what asks - and the reasoning above is
kept because that is the case it still covers.

The mute path was checked for the same shape and is clean.
`NotifySettings::purpleRefreshMute()` goes to `updateLocal()`, which never
issues a request.

`notify_p` is enforced in the private `Data::NotifySettings::isMuted(peer, ...)`,
which is the single root every mute question flows through: the notification
manager, the mute bell in the chat list, sorting, and `History::muted()` - which
is what splits unread counts into muted and unmuted for the badge. Hooking one
accessor gets all of them.

The preset can only ever *add* a mute. A chat you muted yourself stays muted
whichever entry claims it, so switching presets can never un-silence something
behind your back.

### Saved Messages has no exemption

It used to. `VisibleFor()` returned a default `Visibility` for `isSelf()`, which
is `{ Always, notify }`, and `ListFor()` and `ExtraViewHolds()` refused it as
well - so Saved Messages was unconditionally visible and unconditionally
audible whatever the file said. The argument was that nothing arrives in it
unbidden and a chat list that does not show it offers no way back.

Both halves were wrong. It does not offer no way back: Saved Messages is in the
main menu, in search, and behind its own shortcut, none of which the chat list
mediates. And the exemption made it the one chat a preset governed on paper but
not in fact, which is precisely what nobody can reason about from the file.

Worse, a fourth exemption made the first three unfixable. `window_peer_menu.cpp`
skipped Saved Messages when building the `Work Mode` submenu, so the one chat
that ignored the lists was also the one chat you could not put in a list. That
is not a design with an escape hatch; it is a design with a hole.

All four are gone. Saved Messages is an ordinary chat: a preset that does not
name it hides and silences it, and naming it in a list is how you keep it -
which the chat's own context menu will now do for you.
[config.md](config.md) spells that out for whoever meets the empty chat list
first and the reason second.

## Modes that watch unread

`"message"`, `"message_or_reaction"` and `"mention"` make membership depend on
something that moves on its own. A chat appears when it has something to say and
leaves when you have dealt with it, which is the whole idea of a work mode
stated as a rule rather than as a chore.

It is per entry, not per preset. That is what lets one list of groups be gated
while another comes through unconditionally, without a second table saying so -
the same reason every other flag lives on the entry.

`groups_require_mention_p` was the first version of this, and `"mention"` is
exactly what it did. It is retired rather than deleted: writing it warns and
names the replacement, and since a group defaults to `"mention"` now, most files
that used it can simply drop it.

**A chat you are reading does not vanish from under you.** Opening a chat marks
it read, which would take it straight back out of the view. `History::
purpleShowModeSatisfied()` checks `fakeUnreadWhileOpened()` first - the flag
tdesktop already keeps to stop a badge blinking off the moment you open
something - so the messages count as unread for exactly as long as you are
looking at them.

It is off unless an entry asks for it. That default was the other way round
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

Gating is orthogonal to `notify_p`. A gated group with `notify_p = true` still
announces every message, which mostly defeats the point; the combination worth
writing is `notify_p = false` with the gate on, which is "silent, and out of
sight until someone actually wants me". They are kept separate because one entry
decides both, and collapsing them would remove a choice rather than add one.

### Re-checking it

Membership now depends on something that changes constantly, which the chat
list has no reason to re-examine on its own - `shouldBeInChatList()` is not
re-evaluated when an unread count moves.

There are two triggers, because unread arrives by two routes.

**Messages** come through `Data::Changes`. `Entry::notifyUnreadStateChange()`
already fires `HistoryUpdate::Flag::UnreadView` on every unread move, and
`Changes` batches those onto the main loop through `crl::on_main`. That batching
is the point: a hook placed inside `notifyUnreadStateChange()` itself would
re-enter the unread bookkeeping that triggered it, halfway through a walk of
`_chatListLinks`. `Session::setupPurpleWorkMode()` subscribes once.

**Mentions and reactions** also come through
`HistoryUnreadThings::Proxy::setCount()`, which is their own funnel. That hook
stays, sitting outside the `inChatList()` guard and after the existing dispatch,
for two reasons: the chat that needs bringing back is precisely the one that is
not in the list, and for a forum it is that dispatch which rolls the topic's
count up into the parent's sum. It overlaps with the first trigger, and the
extra pass is harmless - refreshing membership twice settles on the same answer.

Both land in `History::purpleRefreshShowMode()`, which is where the cost is
kept down. It returns immediately unless the chat's effective mode actually
watches unread, so a preset built from `"always"` and `"never"` never touches
the chat list at all, and neither does any chat under `normal`.

A transition is logged, by peer id:

    Purple: show_mode 'mention' revealed peer 1234567890.

That is the one event which explains a chat appearing or vanishing with nobody
touching anything, so it is worth a line. It stays rare by construction - only
unread-gated chats reach it, and only on the edge.

One honest hole: under `hide_everywhere_p` the chat leaves the main list
entirely, so `notifyUnreadStateChange()` never fires for it and an unread-gated
chat cannot come back on its own. The combination asks for two opposite things -
"take this out of the app" and "bring it back when it speaks" - so it is
documented rather than engineered around.

### Staying a little longer after you close it

An unread-watching mode has one sharp edge, and it is the edge you meet first:
reading a chat is exactly what takes it out of the view. Click away and it is
gone on that frame. `[recent]` puts a grace period on it.

    [recent]
    stay_visible_after_close = "2m"
    applies_to = "already_in_view"

**The clock starts on close, not on open**, which is the whole difference
between a setting that works and one that does not. Reading something for five
minutes must not burn the grace before you have finished with it; while the
chat is open it stays regardless, and the period begins when it stops being the
active chat.

`applies_to` decides what it covers. `"already_in_view"` is the narrow repair -
only a chat that was in the view when it was opened, so nothing you open can
pull in a chat the preset was hiding. `"any_open_chat"` is the single rule -
a chat you have looked at recently is in the view - and is the only one that
helps when you reach a hidden chat through search or through an extra view.
`"any_open_chat_except_in_folder"` is that, minus the chats already one click
away in this preset: on an extra view, or in a folder whose tab is showing.

Eligibility is decided **when the chat is opened**, not when it is closed, and
that is not an optimisation. Two of the three scopes ask where the chat was at
that moment, and reading it is precisely what moves the answer: by the time you
close it, "was it in the view" has become "was it in the view before I read it",
which is a question nothing can answer afterwards.

Three implementation notes, each of which was the obvious approach's undoing:

- **The seam is `History::setFakeUnreadWhileOpened()`**, above its own guards.
  That setter already *is* "this chat became, or stopped being, the one you are
  looking at" - every caller means exactly that, including the four
  `resetFakeUnreadWhileOpened()` sites, which are all genuine navigations away.
  It simply declines to raise its flag for a chat that arrived with nothing
  unread, and that chat is open just the same. Hooking the flag itself would
  have missed exactly the chats the wider scopes exist for.
- **One timer, on `Data::Session`**, armed for the earliest deadline
  outstanding. Nothing else would ever come back: no message arrives and no
  unread moves when a clock runs out. A `base::Timer` per `History` would be
  thousands of them, and only a handful of chats are in grace at once. The map
  is keyed by `PeerId` rather than by `History*` so an entry left behind by a
  chat that went away cannot dangle.
- **None of it is persisted.** Not `state.toml`, which would then be rewritten
  on every chat switch and would touch the mtime of the file you are
  hand-editing; and not anywhere else either, because a grace period that
  survived a restart would mean the app remembering that you glanced at somebody
  yesterday, which is not what anybody means by "recently".

`purpleShownAsRecent()` is tested before every other rule in
`purpleHiddenFromView()`, because it is not a statement about what the preset
lets through. It re-reads `Purple::RecentStaySeconds()` each time, so turning
`[recent]` off takes effect at once rather than at the end of whatever was
already running.

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

The walk is triggered by `Purple::ActiveChanges()`, which fires when the
resolution differs - and, separately, whenever `settings.toml` changed at all
while a preset is running, even though the resolution came out identical.

That second case is not a belt-and-braces addition; without it the feature was
half broken. A `Resolved` holds the list **names** a preset ordered, as
`EffectiveList { list, show, notify }`, and membership is looked up live by
`MatchList()` against `ActiveSettings()`. So adding a chat to a list - the
`Work Mode` submenu, the main way lists are meant to be edited - produces a
byte-different file and a bit-identical resolution. The equality check returned
early, nobody was told, and the chat sat exactly where it was until some later
change moved the resolution. Switching presets and back appeared to fix it,
which is a good description of a notification bug and a bad description of
anything else.

The two guards on it are the point. A **state** write never forces it:
`UpdateState()` is called from inside `Gate::refresh()` itself to persist the
resolved cache, and from the peek timer, and firing there would rebuild every
chat list for a write nobody can see. And **Normal** never forces it either: an
unconfigured fork must go on paying nothing for a file it is not using, and a
settings change that *starts* a preset moves the resolution and never reaches
that branch.

It logs what it did:

    Purple: preset 'work', 3 lists.
    Purple: 41 of 2251 loaded chats never shown, 203 unread-gated (2 showing), 0 silenced, view holds 262.

Unread-gated chats are counted apart from hidden ones because it is a different
claim. A gated chat is only out of the list while it has nothing to say, so the
number that means anything is how many of them are still showing - and a zero
gated count means the gate is off rather than that it found nothing.

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

A preset names which chat folders appear, in the order it names them:

    [presets.work]
    folders = [ { name = "Work" }, { name = "Uni" } ]

Saying nothing about folders shows none, which collapses the folder strip
altogether. That is the same rule the lists follow - a preset names what it
wants - and it is why there is no "said nothing" case left to distinguish from
an empty one.

To get the whole strip back, ask for it:

    folders = [ "*ALL" ]

`"*ALL"` is the one built-in set: every folder the selection does not name
elsewhere, at that position, with default flags. It is the only spread the
parser cannot expand, because the parser has never heard of a Telegram folder -
it survives as a folder entry holding that exact name and
`ChatFilters::purpleRefreshShown()` expands it in place, which is what keeps its
position in the strip meaningful. So `[ { name = "B", ... }, "*ALL" ]` reads as
"B on my terms, then everything else on default terms".

Each named folder carries five flags. `show_p` puts its **tab** in the strip and
defaults to true, since naming a folder is normally how you ask for it -
`show_p = false` is for a folder you want silenced or pulled into the view
without its tab being there. `notify_p`, `show_mode` and `include_in_main_view`
are below, and all three are about the folder's **chats** rather than its tab.

`enabled_p = false` is the fifth, and it is about the entry rather than about
either. It makes the preset ignore the whole thing - no tab, nothing silenced,
nothing pulled in, nothing counted - while the entry keeps everything configured
on it. It is for a preset you are still tuning, where deleting a folder's
settings to try life without it means writing them again afterwards.

A disabled entry stays in the *resolution*, rather than being filtered out of
it, and the reason is `"*ALL"`. That marker is expanded late, against the
account's real folders, and skips whatever the selection already names. Filter
the disabled entry out early and `"*ALL"` no longer sees the name, so it hands
the folder straight back - switching a folder off would silently stop working
the day the preset gained a spread. So the entry stays, claimed and inert, and
every place that acts on a folder asks `FolderEnabled()` first.

Names are matched against folder titles, case-insensitively. A name matching no
folder is skipped and logged, for the same reason the hidden-chat count is
logged - a folder named slightly wrong is indistinguishable from a folder the
preset meant to leave out.

"All chats" is dropped, and the preset view stands in its place - see
[The preset view](#the-preset-view). It is the only tab that shows a chat
belonging to no folder, so something has to be there, and the view is what the
preset means by "the chat list" anyway. It is also what makes a preset with no
folders work without a special case: left as the only entry it takes the strip
below its "more than one tab" threshold and the strip hides itself.

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

The two drag handlers also bail out early. Better than that, both mark the whole
strip as a pinned interval while a preset restricts it, so `Ui::Reorder` never
starts the drag at all: the tab does not lift, rather than lifting, animating
back and leaving nothing to explain what just happened.

A preset whose whole folder selection is `"*ALL"` still allows reordering. Its
shown list is the real one with the view standing exactly where All chats stood,
so every index still means the folder it did before. Any other shape - a subset,
a chosen order, a folder carrying flags - means a strip index no longer matches
the server's, and `saveOrder()` refuses.

Extra views break that alignment too, whatever the folder selection says: a
preset's own tabs stand between the main view and the first folder, so the
one-for-one swap of All chats for the view is no longer what the strip is. A
preset declaring any view is treated as restricting folders, and a peek does not
lift it - a peek reveals folders and leaves the extra views exactly where they
were, so the arithmetic stays wrong.

To reorder folders while a preset restricts them, switch to `normal` first.

### Pulling a folder into the view

    folders = [ { name = "Family", include_in_main_view = "all" } ]

An escape hatch, and the positive form of what used to be spelled
`filtered = false`. The chats in that folder join the preset's view whatever the
lists decided.

What it does **not** decide is *when* they show. A folder that names no
`show_mode` leaves its chats to the default for what each one is - so a channel
filed there is simply present, and a group filed there still waits to be
mentioned. The folder chose which chats come in; the kind still decides when.
Write `show_mode` on the folder to answer both at once.

The polarity is worth the rename. `filtered = false` described the mechanism
from the inside - "this folder is not subject to filtering" - and left you to
work out what appeared where. `include_in_main_view` says what happens.

It takes three values, and no `_p` suffix, because it is not a yes-or-no:

- `"none"` - the default. The folder's chats are left to whatever their entry
  decided, which is what every folder the preset does not name is left to.
- `"pinned"` - only the chats pinned *inside that folder*.
- `"all"` - everything in it.

The two spellings this replaced, `include_in_main_view_p` and `pinned_only_p`,
are retired keys that warn with the replacement. Accepting them quietly would
be worse than breaking them: a folder still saying `include_in_main_view_p`
would include nothing and look exactly like one that meant to.

Note what a folder tab shows, because it changed with the view. The preset
decides the view; a folder decides its own tab. So a chat the preset hides is
missing from the view and still present inside its folder. That is the strict
reading, it is what "a folder shows what the folder says" implies, and it was
not affordable before the view existed.

If a folder's contents do not belong in a work mode, do not name the folder -
that is what naming folders is for. `include_in_main_view` is the opposite
lever: it pulls the folder's chats *into* the view.

This was left open for a long time, as "should a folder tab be filtered by the
preset too", and is now settled: **it should not**. Opening a folder is a
deliberate act. You went there, by name, having chosen which folders the preset
shows at all - and a preset that then second-guessed you inside the folder would
leave nowhere to look at the thing you went looking for. The strict reading is
also the useful one, and the lever for the other behaviour already exists: drop
the folder from `folders`.

Under `hide_everywhere_p` the old rule comes back, and must: a chat out of the
main list cannot be in a folder either, because tdesktop guarantees the reverse
throughout - `Entry::notifyUnreadStateChange()` asserts on it outright. So a
pulled-in folder's chats stay visible everywhere, and a hidden chat is gone from
its folder as well.

The lookup is free for everyone who does not use it. It runs only for a chat
the preset would otherwise hide, and only when some folder actually asked, so a
preset with no exemptions never walks the folder list at all.

#### It overrides the archive

Whatever a folder lets in comes in **even when the chat is archived**.

Archiving is how visibility gets controlled in stock Telegram - you archive a
channel to get it out of the way. Under a preset the preset controls visibility,
precisely and by name, so a folder that asked for its chats should get them
wherever they happen to be filed. Otherwise you would have to unarchive things
to make a preset work, which is editing the account to change a view.

The chats stay archived. They are simply also in the preset's view, exactly the
way a real Telegram folder holds archived chats unless it sets `NoArchived`.

This one needed care, because it looks like it should break tdesktop's "in a
filter implies in the main list" invariant. It does not:
`Entry::notifyUnreadStateChange()` asserts `inChatList()`, and for an archived
chat that means *the Archive's* list, which it is in. Two places knew otherwise
and were taught:

- `Session::refreshChatListEntry()` excluded `entry->folder()` from the view
  outright, mirroring All chats. It now asks `purpleShownFromArchive()` first.
- `Session::refreshPurpleView()` walked only `_chatsList`, so nothing would ever
  visit an archived chat on a preset change. It walks the Archive's list too.

The unread arithmetic works out without a change, which is worth stating because
it looks like it should not. The Archive *row* is in the view and carries the
whole archive's unread; `Data::UnreadStateValue()` already subtracts the archive
total to stop it being counted twice. Adding some archived chats individually
therefore leaves them counted exactly once: `normal + archive + pulled - archive`.

#### Only the pinned ones

    folders = [ { name = "Music", include_in_main_view = "pinned" } ]

For a folder you keep a handful of current things at the top of, this is the
difference between "the two albums I am listening to" and every channel ever
filed there.

"Pinned" is the folder's own pinned order, `ChatFilter::pinned()`, which is what
Telegram itself puts at the top of that tab - not the main chat list's pins and
not a preset view's. A folder with nothing pinned in it therefore contributes
nothing, which is worth knowing before wondering where the folder went.

The narrowing is per folder, and a chat filed in two exempt folders needs only
one of them to let it through: a narrow folder saying no does not speak for a
wide one that would say yes.

What it does **not** touch is the folder's own tab. That still shows the
folder's real contents, pinned or not, for the same reason as everything above -
the preset decides the view, a folder decides its own tab.

### Leaving a folder out of the counts

    folders = [ { name = "Music", badge_p = false } ]

No number on that folder's tab, and its chats left out of the app badge. For a
folder that is background on purpose, a count is a number you have already
decided not to act on, and the dock icon claiming attention on its behalf is
exactly the interruption a work mode exists to stop.

It is a third axis, independent of the other two: a folder can be silenced
without being uncounted (you want the number, just not the sound) and uncounted
without being silenced. Only an explicit `false` does it; saying nothing leaves
a folder counted, which is what almost every folder wants.

The tab half is easy - `Data::UnreadStateValue()` returns an empty state for
such a folder, re-asked on every resolution change because the answer lives in
`settings.toml` and the strip is not rebuilt when only a flag on a tab moved.

The app-badge half is the interesting one, because it is a *subtraction*, and
the last subtraction in this file drove a badge to `-334`. The rule that keeps
it honest: **subtract a subset, never an estimate.** There is a second reserved
filter id, `Data::kPurpleQuietFilterId`, just past the view run, holding exactly
the main view's members that sit in an uncounted folder. It is never a tab and
never in `purpleShownList()`; it exists so that

    app badge = view unread - quiet unread

cannot go negative, because the quiet list is a real `Dialogs::MainList` whose
total is accumulated through the same `addEntry`/`removeEntry` path as the list
it is taken out of. Its membership is maintained in
`Session::refreshChatListEntry()` immediately after the views, so it is a subset
by construction rather than by argument.

`Session::purpleBadgeUnread()` is the one place the subtraction happens, and all
seven app-badge accessors go through it - one place to be wrong rather than
seven.

A peek does not lift it. A peek is a look at the chat list; it is not a request
to be interrupted on behalf of a folder you asked to keep quiet.

One thing it deliberately does not touch: the preset's own view badge. A chat an
uncounted folder pulled into the view still counts toward the tab standing where
All chats stands, because that tab is counting what is on screen in front of
you, which is a different question from whether the app icon should light up.

### Silencing a folder

    folders = [ { name = "Noise", notify_p = false } ]

The chats in that folder are silenced, on the same terms as a silenced entry:
the preset only ever adds a mute, so a chat muted by hand stays muted whichever
folder it is in.

A list can already do this when the folder is a hand-picked set - the same ids
with `notify_p = false` - and for those the list is the simpler tool. What a list
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
`notify_p = false`, and that emptiness is checked first, so every mute query in
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

A preset can be deleted or renamed while it is active, or the file can stop
parsing mid-edit. The engine then runs on `resolved_cache` in `state.toml` - the
last resolution that worked - rather than falling back to defaults.

This is deliberate and worth stating plainly: defaulting would unhide every chat
you had hidden, which is the one outcome a work mode must never produce by
accident. If there is no cache either, the resolution already in effect stays in
effect and the reason is logged.

The cache carries the resolved entries in order, each with its `show`, `notify`
and mention gate; the folder selection, marker included; the tab's name; and any
extra views, their pins among them. In short, everything a reload would
otherwise take away - the point being that a `settings.toml` broken halfway
through an edit changes nothing you can see.

List *membership* is not cached, and this is the assumption to watch: it comes
from `settings.toml`, and the argument that it need not be cached is that a file
which did not parse leaves the previous settings standing in memory.

That argument holds only while the process is still running. It does not survive
a cold start, and it does not survive the file going *missing* rather than going
bad - and with no settings at all, no list claims anything, so a preset that
names what gets through hides the entire account. The cache faithfully restores
an order that now refers to lists nobody can look up.

The Android fork closes this by keeping `settings.toml.good`, a copy of the last
file the core accepted, and parsing that when the real one is missing or
unusable. Keeping the file rather than extending the cache is deliberate: it
needs no second schema, it cannot disagree with the real file about what a list
means, and it covers both failures with one mechanism. The picker says when it
is running from the copy, because a preset resolved from a file the user cannot
see should not be a silent state. The desktop has the same shape on a cold start
and does not do this yet.

## Choosing a preset

The main menu carries the switch, above Settings rather than inside it: a work
preset is changed several times a day and settings are not. The entry reads
`Work Mode` under Normal and `Work Mode: work` otherwise, so the current mode is
legible without opening anything.

Settings > Advanced > Purple carries a second entry to the same box. Not because
the main menu is hard to reach, but because Settings is where someone looks for
a feature they have heard of and cannot find, and a search for "work mode"
turning up nothing is how a feature stays unused.

The box lists Normal and every preset in the file, each with a summary of what
it does - `work  -  lets through 3 lists, silences 1 list, 2 folders` - built by
resolving the preset rather than by describing what was typed. Choosing one
applies it immediately and leaves the box open, so a wrong guess is one click
from being undone.

Those rows wrap. A `Ui::Checkbox` is one elided line by default, which is fine
for `Show tray icon` and wrong for a sentence: elided, every preset read
`lets through 3 lists, silences 1 li...`, which is worse than no summary at all
because it looks like the whole answer.

The summary counts what gets *through*, not what is hidden. Under this model
counting the hidden would be counting the whole account, since anything a preset
does not name is hidden by falling through.

### What it is doing, as opposed to what it says

Under the rows, while a preset is running, is the one line the file cannot
give you:

    Showing 535 of 641 loaded chats.
    People 128, Inner 3.

The summaries above it are read off `settings.toml` and would look exactly the
same if a list name were misspelled - the preset would resolve, name a list that
holds nobody, and quietly hide everything. This line is read off the chat lists
themselves, so it is the answer to the question anyone actually has.

It is refreshed on `ActiveChanges()` but through `crl::on_main`, because that
signal is what the session subscribes to in order to *rebuild* those lists.
Counting from inside the handler would count whatever the previous preset left
behind.

`state.toml` remains the source of truth and is still hand-editable; the box is
a second writer to the same field, not a replacement for it.

### Reaching the file

The line naming `settings.toml` at the bottom of the box is a link, and clicking
it reveals the file in the system file manager. Everything the box cannot do -
writing a preset, editing a list, naming a folder - is done in that file, and
until it was clickable, getting there meant retyping a path out of a label.

It reveals rather than opens: `.toml` has no registered handler on most installs,
and the file opening in whatever happens to have claimed the extension is a
worse surprise than a file manager window.

### What the box says when something is wrong

`settings.toml` problems - the error, and every warning - are shown here. Until
now they only reached the log, which meant a preset that silently did nothing
because of a mistyped list name looked exactly like a preset that was working.

Errors are drawn in the colour the rest of the app uses for something that needs
attention; warnings stay in the muted text of a note. The difference is the
whole point of having two words for them - an error means the file did not load,
a warning means it loaded with something ignored - and in one column of body
text they read identically. The `Error:` and `Warning:` prefixes stay, so this
does not rest on colour alone.

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

Three things the submenu deliberately does:

- **It offers every list**, including one that matches by `kinds`. Adding a chat
  to a rule-based list writes an explicit member id, which is how you pull one
  chat out of a rule that would otherwise have swept it up somewhere else.
- **It does not appear at all** unless you have written a list, so an
  unconfigured fork's menus are exactly upstream's.
- **It names what is deciding the chat**, while a preset is running: a first
  line reading `In 'Essentials': shown` or `In 'bots': hidden`, which opens the
  preset box. That is the question that brings anyone to this menu.

When no entry claims the chat the line says so - `In no list 'work' names:
hidden` - because that is a real answer under this model rather than a gap.

The line reports what actually happened to the chat, not what the entry asked
for. The two come apart for a chat a folder pulled back in: the entry says hide,
`include_in_main_view_p` says keep, and there it reads
`In 'bots': silenced, shown by a folder`. Printing the entry's verdict instead
would put the word `hidden` over a chat sitting in the list, which is worse than
saying nothing. A gated group reads `hidden until a mention` for the same
reason: that is where it stands right now.

Membership is global, not per preset. A chat is in Essentials or it is not; what
changes between presets is what Essentials *does*, and whether the preset names
it at all. The line above tells you which entry won, which matters because
priority is per preset now - the same chat in the same two lists can be decided
by a different one under a different preset, and adding it to a list that entry
ranks below one already holding it changes nothing there.

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
preset. `Ctrl+Shift+E` suspends the hiding for two minutes: every chat is back
in the list, no group is waiting for a mention, and every folder is in the
strip. Press it again to end it early.

    [peek]
    hotkey   = "Ctrl+Shift+E"
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

The key is named in *native* text. The file holds Qt portable text, because that
is what `QKeySequence` parses and what these docs can describe once for every
platform, but on macOS Qt reads `Ctrl` as Command - so a label repeating the file
would print `Ctrl+Shift+E` next to a key that does nothing. The checkbox runs the
sequence back through `QKeySequence::NativeText` and shows what the keyboard has
on it.

While a peek is running the same checkbox counts it down - `Peeking - 1:23 left`,
or `Peeking - until you turn it off` when `auto_off` is off. A peek ends on a
clock and nothing anywhere said when: the toast at the start was the only
warning, so chats reappearing and then going again two minutes later had no
visible cause. The timer runs only while the box is open, which is the only time
there is anyone to read it.

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
    enabled_p    = true
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

Turning `enabled_p` off while focus is holding a preset hands that preset back. A
preset that nothing on screen explains and nothing still running would ever lift
is the one state this must not be able to reach.

## Verified, and not verified

Most of this has been exercised on a real account with a live log line to read
off - `N of M loaded chats never shown, K unread-gated (J showing), view holds
X` is the regression check, and switching a preset off and on again prints it.

The three things this section used to list as unverified were measured on
2026-08-24. All three held, and the third turned up something.

- **The app badge with `badge_p = false`: measured.** The window title carries
  the badge, through `Session::unreadBadge()` and so through
  `purpleBadgeUnread()`. With the Music folder pulled into the view, the title
  read **2,005,908** with `badge_p = false` and **2,244,361** with it true. The
  quiet-list subtraction is real, not an argument.

  Note the setup that makes it measurable at all: a folder whose
  `include_in_main_view` is `"none"` contributes nothing to view 0, so the quiet
  list is empty and `badge_p` has nothing to subtract. The flag only bites for a
  folder whose chats are actually in the view.

- **A gated chat leaving the view after being read: confirmed.** With `[recent]`
  off, marking a close person unread put them in the view, and opening them and
  then clicking away took them out again on that frame.

  "Mark as unread" is the way to test this without waiting for someone to write
  to you: `purpleShowModeSatisfied()` counts `state.marks` as a message on
  purpose, and opening the chat clears the mark, which is exactly the transition
  in question. It is also reversible, unlike reading somebody's actual messages.

- **Which trigger fires for a plain message: `refreshHistory()`, and the log
  line is misleading about it.** Neither the entering nor the leaving transition
  above printed a `show_mode ... revealed/hid` line. That is not a failure - it
  is `Entry::notifyUnreadStateChange()` calling
  `chatsFilters().refreshHistory()` first, which reaches
  `Session::refreshChatListEntry()` and fixes the membership before the
  `Data::Changes` subscription runs. By the time `purpleRefreshShowMode()`
  looks, `inChatList(view)` already matches and there is nothing to report.

  So on an account with real folders the `Data::Changes` hook is a no-op for a
  plain message, and the transition log is effectively dead. It is still not
  redundant in general: `refreshHistory()` returns early on
  `list().empty()`, so on an account with no folders at all the subscription is
  the only path. That half is read off the source rather than measured - it
  needs an account with no folders, which this is not.

## A chat with no conversation

A person you have never messaged is exactly who you want one click away, and
until this was fixed they could not be. Naming an id in a list did nothing for a
contact with an empty chat: they were simply absent from every tab, with no
warning and no way to tell that from a typo.

Two separate gates were dropping them, and forcing either one alone looks like a
fix and changes nothing:

- `History::shouldBeInChatList()` ends at
  `!lastMessageKnown() || (lastMessage() != nullptr)`. An empty conversation is
  false.
- Even past that, `Entry::setChatListExistence()` is
  `if (exists && _sortKeyInChatList)`, and the key comes from
  `DialogPosFromDate(adjustedChatListTimeId())`, which returns **0** for date 0.
  A chat with no messages has no date, so it has no sort key, so it is refused
  however loudly the first gate says yes.

Failing either sends the entry to `removeChatListEntry()`, which never reaches
the extra-view loop in `Session::refreshChatListEntry()`. Hence "in a list, on
no tab".

Both are now overridden for a chat the preset names, through
`History::purpleKeptForView()`: the `shouldBeInChatList()` override goes **last**
so every structural check above it still applies - a channel you have left stays
out, and so does a chat whose folder is unknown, which `refreshChatListEntry()`
asserts on - and `adjustedChatListTimeId()` returns `TimeId(1)` instead of 0,
which is a real position that sorts below every real chat. That is where a
conversation which has not happened yet belongs.

### Named, not merely matched

The guard is `Purple::NamedExplicitly()`, and it is the whole safety of the
thing. It asks whether the id is **written out** in the `members` of a list the
preset or one of its views orders - not whether some list matches the chat.

A `kinds` rule describes a category; `members` names a chat. Only the second is
a reason to keep an empty chat in the chat list. Read a `kinds` match as an
explicit request and `{ kinds = ["private"] }` drags every contact you have
never messaged into the list, which on this account is most of nineteen hundred
of them.

Entries whose `show_mode` is `"never"` are skipped too: naming a chat in order
to hide it is not asking for it to be anywhere. The predicate deliberately does
not model the main order's first-match-wins capture - over-approximating costs
nothing, since the only consequence is keeping a row for a chat the user typed
in by hand.

## A folder that silences, and a chat that drifts into it

A rule-based folder computes membership from the chat's own properties, so a
chat can cross into one with nothing announcing it - and `History::muted()` is a
cache, so it would go on ringing.

`NotifySettings::purpleRefreshFolderMute()` keeps the set of peers
`purpleSilencedByFolder()` last said yes to, and re-evaluates the mute when the
answer flips. It is called from `Session::refreshChatListEntry()`, next to the
quiet-list block and for the same reason: that is where membership is already
being recomputed. A preset that silences no folder pays one empty-vector test.

The refresh is deferred through `crl::on_main`. Re-evaluating a mute moves the
chat's unread through every list holding it, and doing that inside the walk
currently placing the chat in those lists is how the totals drift - the same
hazard the mute-before-hide ordering in `refreshPurpleWorkMode()` exists for.

## Stories

The strip follows the preset, and the interesting part is what "follows" means.

A chat is out of the view for one of two quite different reasons. Either the
preset **excludes** it - no list claims it, or one claims it with
`show_mode = "never"` - or the preset **admits** it and it happens to be quiet,
which is what `message`, `message_or_reaction` and `mention` say. Only the first
of those hides a story. A story *is* new activity, so a person admitted under an
unread-watching mode keeps theirs; suppressing the one thing they do have would
invert the setting that let them in.

That is `follow`, the default. `all` skips the peer filter, `all_unseen` and
`follow_unseen` add a seen filter on top, and `none` turns the strip off. A
`list_order` entry or a folder overrides it for its own people with `always`,
`unseen` or `never` - a narrower vocabulary on purpose, since "all" would be a
category error on something that is already a set of people. A folder beats an
entry, and both beat the policy, matching the order already used for hiding.

Three implementation choices worth recording.

**Filtered in `State::next()`, not in `Data::Stories`.** The latter also feeds
the story counters, the archive strip and the upstream hidden/unhidden
machinery, none of which has anything to do with a work preset - filtering there
would be lying to code that never asked. `dialogs_stories_content.cpp` is the
one place that is only about the strip. `Content::total` moved to after the loop
at the same time, or the strip would report a count it is not showing.

**`hasUnseen` is passed in rather than looked up.** Each source already carries
`unreadCount` right where the filter runs, so the seen half of the ladder costs
nothing and the gate needs no access to story state at all.

**The producer merges `Purple::ActiveChanges()`.** `ContentForSession()`
subscribed only to `sourcesChanged`, and nothing about the sources changes when
a preset does, so the strip would have kept whatever it was showing until the
next story arrived. Merging into the existing producer rather than re-firing
`_storiesContents` from `Dialogs::Widget` keeps `State` alive, and with it the
userpic cache that would otherwise be thrown away on every preset change.

The name avoids "hidden" throughout - `StoryShown`, not `StoryHidden` - because
upstream already has hidden stories, meaning the ones you moved to the archive
strip yourself. Two different ideas with one word is how they eventually get
wired together by mistake.

## Hotkeys

`[peek] hotkey` binds the peek key; `hotkey` on a preset binds that preset.

### Keys a message field has already taken

The peek key used to default to `Ctrl+Shift+E`'s predecessor, `Ctrl+Shift+P`,
and it did not work - but only while the composer had focus, which made it look
like a focus bug rather than a clash. It is neither.

`Ctrl+Shift+P` is the **spoiler** shortcut. `Ui::InputField` registers ten
markdown sequences (`kSpoilerSequence` and friends, in lib_ui's
`input_field.h`) as `Qt::WidgetShortcut` on the field itself, so they join the
contest only while a field has focus. Qt then sees two claims on the sequence,
declares it ambiguous, and fires **neither** - silently, with nothing in the
log. Measured rather than reasoned about: an event filter logging
`QShortcutEvent::isAmbiguous()` showed `ambiguous=0` with no chat open and
`ambiguous=1` with the composer focused, and `triggerPeek()` never running in
the second case.

Two things came out of it. The default moved to `Ctrl+Shift+E`, which nothing
claims. And `ParseSettings()` now warns when any configured hotkey is one of
the ten, naming the formatting action it collides with - because the failure is
otherwise invisible, and "it works until I click the message box" is a terrible
thing to have to debug twice.

The reserved sequences are `Ctrl+B`, `Ctrl+I`, `Ctrl+U`, `Ctrl+K`, and
`Ctrl+Shift+` with `X`, `M`, `N`, `P`, `D` or `.`. They are compared as
normalised text rather than as `QKeySequence`, because `purple_settings.cpp` is
compiled standalone against Qt Core and `QKeySequence` is QtGui.
Pressing a preset's key while it is already running turns it off rather than
doing nothing, so one key means both "get to work" and "come back" - a key that
is a no-op half the time reads as broken.

They are not `Shortcuts::Command`s. That table is owned by
`tdata/shortcuts-custom.json` and by the shortcuts settings page; these keys are
owned by `settings.toml`, where the rest of a preset lives. Two files claiming
one binding is the situation the config split exists to avoid.

`Purple::ListenHotkeys()` holds one `QAction` per binding for the whole
application and re-points them when the file changes, rebuilding the set only
when a signature of the declared keys actually moves. The parser refuses a
duplicate key, and that is not tidiness: two actions holding the same sequence
make it ambiguous and Qt fires **neither**, so a silent duplicate breaks both
keys rather than picking a winner.

The format is documented in [config.md](config.md), including the macOS trap
that `Ctrl` is Command and `Meta` is the physical Control key.

## On Android

The Android fork runs the same core, compiled through the NDK into
`libpurplecore.so`, so a `settings.toml` written on the desktop means the same
thing on the phone. What is ported so far is the chat list and silencing: a
preset decides what is in the list and what may interrupt you, and a picker
chooses the preset.

Three things make that possible, and they are the contract between the two
apps rather than an implementation detail of either:

- **Four kinds and nothing else.** A bot, then a user, then a broadcast
  channel, then everything else is a group. Basic groups and supergroups are
  one kind on both, because being upgraded must not move a chat between lists.
- **The bare id.** The file keeps the peer id with its type stripped, which on
  Android is the absolute value of the dialog id. There is no `-100` channel
  prefix in the Android client; that is the Bot API's convention, not this
  codebase's. An encrypted chat resolves to the user behind it.
- **The engine answers with a `show_mode`, not a yes.** The unread half is
  finished by the app, against its own unread counts, exactly as the desktop
  finishes it in `purpleShowModeSatisfied()`.

The Android seam is `PurpleGate.java`, which is `purple_gate.cpp`'s opposite
number: the one file that knows both what a `TLRPC.Dialog` is and what the core
wants. It hides by leaving rows out of the list `DialogsActivity` draws, never
by touching the model, so a hidden chat is still pinned, still in search and
still in the forward picker.

Two deliberate narrowings, both taken from this document:

- **The archive and the folder tabs are not filtered.** A preset decides its
  own view; a folder decides its own tab. *Which* folders a preset shows is the
  `folders` key and is ported; what is inside one is the folder's business.
- **Pin dragging is refused while a preset runs.** Android turns a drop into a
  server-side order by reading the model list and sending it with `force`, so
  dragging inside a filtered list would drop the hidden chats' pins from the
  account and from every other device. That is the same hazard the desktop
  guards in `ChatFilters::saveOrder()`, met in a different place.

### The folder strip

The two-accessor rule is the desktop's, met in Android's shape.
`MessagesController.getDialogFilters()` is now a display-only view - the folders
the preset named, in the order it named them - and
`getDialogFiltersUnrestricted()` holds what that method used to be. Everything
that edits a folder, counts them, resolves membership or uploads an order reads
the second one, or the folder settings page, the add-to-folder menu and the
premium limit start describing an account the user does not have.

One divergence, and it is the interesting one: **"All chats" always leads the
strip.** The desktop drops it, because the preset's own main view stands in its
place. Android has no extra views yet and does not need one here - the default
tab already draws the preset's filtered list, because the hiding happens inside
the list `DialogsActivity` builds rather than in a separate view object. So on
Android All chats *is* the preset's main view, and taking it off the strip would
leave nowhere to see the preset at all.

One footgun, shared with the desktop and worth stating out loud because it
fails quietly: a folder is named with a table, `{ name = "News" }`. A bare
string in `folders` is a `"*set"` reference and nothing else, so
`folders = [ "News" ]` names no folder — it warns and shows nothing. `"*ALL"`
is the one bare string that means something there.

`"*ALL"` is expanded on the Java side, in place, exactly as
`ChatFilters::purpleRefreshShown()` expands it: the core has never heard of a
Telegram folder, so the marker arrives as a marker. A named folder that matches
nothing is skipped and logged, because a folder named slightly wrong looks
exactly like one the preset meant to leave out. And a preset that says nothing
about folders shows no folder tabs at all - the same rule as the desktop, and
the reason `"*ALL"` exists.

**Folder reordering is refused while the strip is restricted**, in three layers:
the drag itself (`FilterTabsView.TouchHelperCallback.getMovementFlags`), the
Reorder menu entry, and the two adapter methods that rewrite the real filters'
`order` fields in place. Android is worse than the desktop here rather than
merely different: `TL_messages_updateDialogFiltersOrder` sends the complete
positional id list, *and* the drag mutates the accessor's return value before
uploading it. A restricted copy would silently lose the reorder; a restricted
view would corrupt real order values. The guard keys off "the strip is not the
account's own folders in the account's own order", not off "a preset is
running", so `folders = [ "*ALL" ]` leaves dragging alone - the same test as
`Purple::FoldersRestricted()`.

Switching presets forces the selection back to All chats. The tab the user is on
is an index into a list whose membership just changed, and the stable-id walk in
`updateFilterTabs` only rescues a tab that left the strip - an index that is
still in range quietly means a different folder.

### Three mute roots, not one

Silencing is the one place where the port could not follow the desktop's shape.
The desktop has a single root - `Data::NotifySettings::isMuted` - and hooking it
made the bell, the sorting, the notifications and the badge follow at once.
Android decides muted-ness in three separate places, and two of them have to be
hooked:

- `MessagesController.isDialogMuted` reads the `notify2_` preferences and is
  what every visible surface asks: the bell in the chat list, the grey unread
  counter, the folder "exclude muted" predicate, and the muted buckets the
  unread counters are built from.
- `NotificationsController.getNotifyOverride` decodes the *same* preferences
  again, independently, and is the only thing the delivery path consults -
  `NotificationsController` never calls `isDialogMuted` when deciding whether to
  ring. Without this second hook a preset would show the muted bell and count
  nothing while the phone still rang.
- A SQLite mirror, `dialog_settings.flags`, is read once at cold start to decide
  which chats are eligible for a push. This one is deliberately left alone: it
  runs on the storage thread before the users and chats are loaded, so the gate
  could not tell a bot from a person there, and the counts it seeds are
  corrected by the first counter pass anyway.

The rule is the desktop's, unchanged: **a preset only ever adds a mute.** A chat
you muted by hand stays muted whichever entry claims it, and switching presets
never un-silences anything. That makes the split the desktop needs necessary
here too. `MessagesController.mutedWithoutPreset` is Android's
`purpleMutedWithoutPreset`, and everything that labels or drives a Mute/Unmute
toggle reads it: the chat list's long-press menu and swipe label, the action
mode's mute icon, the notification popup, the chat header, the profile row, the
topic menus, and the notification-exceptions list, which is a list of your own
exceptions by definition. Everything asking whether a chat is *quiet* - the
bell, the grey counter, the title icons, the counters - keeps the effective
answer.

The exclude-muted folder predicate reads it for a different reason, and it is
the same loop this document describes under "Breaking the loop": membership is
decided as though this preset silenced nothing, or a folder that excludes muted
chats would eject a chat the moment the preset silenced it, which would
un-silence it, which would put it back.

One consequence worth stating, because it looks like a bug and is not: a
preset-silenced chat stays *in* an exclude-muted folder but drops out of that
folder's unmuted count. The folder still holds the chat; the badge no longer
shouts about it. That is the same asymmetry the desktop has.

The picker lives in the chat list's overflow menu, labelled with the running
preset, and again in Settings - for the same reason it is in two places on the
desktop. When the active preset is not in the file, no row is checked and the
box says so, because checking Normal would fire the selection callback and
unhide everything over a typo.

### Putting a chat in a list

Ported, and it is the same splice: selecting one chat and choosing **Work Mode
lists** offers every list in the file, checked where the chat is already a
member, and a tap adds or removes it. The line the splice writes carries the
chat's name as a trailing comment, regenerated from the model rather than read
back out of the file, and every other byte - comments, ordering, blank lines -
is left where it was.

The naming callback the desktop passes as a `std::function` becomes a JSON map
of id to name, handed over with the request. Calling back into Java per id would
mean holding a `JNIEnv` across the splice; the caller already knows the names of
everyone in the list, so it sends them.

Two divergences, both about where the menu lives rather than what it does. It
hangs off the selection mode's overflow, next to *Add to folder*, because
Android has no right-click and the preview menu that would be the closer
analogue is not reachable on the test emulator - shipping UI that cannot be
verified is not worth the closer match. And it is offered for **one chat at a
time**: membership is a property of a chat, and a tick meaning "some of these"
would have no honest answer.

The rest is the desktop's, unchanged. Every list is offered, including one that
matches by `kinds`. Nothing appears at all unless a list has been written.
Membership is global, not per preset. And a failed splice - an unwritable file,
or a list written as an inline array it will not edit a line at a time - leaves
the file and the running resolution exactly as they were.

Not ported: the line naming what is currently deciding the chat
(`In 'bots': hidden`). That needs the resolution to say which entry won, which
is a question the bridge cannot answer yet.

### Not ported yet

Extra views, overrides, peek, the schedule and the `[recent]` grace period.
Hot reload and the list-membership menu are done; see above.
The `folders` key itself is complete: the tab, `notify_p`, `badge_p` and
`include_in_main_view` all work.

A folder's `notify_p` is ported. A folder silences its chats here the way it
does on the desktop and by the same shape: `PurpleGate.silencedByFolder()` is
`NotifySettings::purpleSilencedByFolder()`, matching the preset's silenced
folder names against the account's real folder titles and asking each match
whether it holds the chat. Answered live on every query rather than from a
snapshot - folder membership moves with every message that arrives, so there is
nothing worth caching - and behind the same emptiness check the desktop puts in
front of its own walk, so a preset that silences no folder never reaches it.

The snapshot was the tempting design and it is the wrong one here.
`sortDialogs()` maintains a folder's membership list only for the tabs currently
on screen, and returns immediately while the interface is paused, so a cached
set would be stale in exactly the case that matters - a notification arriving
with no chat list open.

This is what makes the exclude-muted loop described above real on Android for
the first time, and the breaker put in place pre-emptively is now load-bearing:
`DialogFilter.includesDialog()` reads `mutedWithoutPreset`, so membership is
decided as though this preset silenced nothing.

Two limits worth stating. The walk is reached from the storage and
notifications threads and it reads lists the UI thread owns; a walk that throws
answers "not a member", which is the safe direction - the preset silences
nothing it was not already silencing, and the next query gets it right. And a
folder can only answer once the folder list has loaded, so the first push after
a cold start, arriving before the dialog list, is not folder-silenced. The
desktop has the same hole for the same reason and says so.

`badge_p` is ported too, and it is the same query asked of a different path.
The two branches of `getTotalAllUnreadCount()` that already walk every dialog
get the check next to the one that skips hidden chats. The other two take
`total_unread_count` and `pushDialogs.size()`, both maintained incrementally in
five places each, so taking an uncounted folder off them would be a subtraction
- and the last subtraction in this document drove a badge to `-334`. Android
gets the same guarantee by a cheaper route than the desktop's reserved filter
id: the totals are **rebuilt** over `pushDialogs`, the very collection they are
accumulated from, so the answer is a subset by construction and cannot go
negative. The rebuild only ever runs when a preset actually named such a folder.

The tab half is one line, and the rule about what it does *not* touch is the
desktop's: a chat in an uncounted folder still counts toward All chats, because
that tab is counting what is on screen in front of you, which is a different
question from whether the launcher icon should light up.

`include_in_main_view` is ported, and with it the folder mechanism is complete.
The walk is the desktop's `History::purpleExemptFolderMode()`: `"pinned"` reads
the folder's own pinned order, a chat in two exempt folders takes the most
permissive answer, and a folder that names no `show_mode` leaves its chats to
`DefaultShowMode()` for whatever they are - which the bridge hands over as four
numbers rather than a second native, since a chat asking for its own default is
on the row-drawing path. The folder decides first when it claims a chat at all,
the way `purpleHiddenByPreset()` orders it: it named this folder, where a list
entry named a kind or a set of ids.

Two things it does not do, and both are worth saying because they look like
bugs.

**A pulled-in chat is visible, not un-silenced.** The notify half still comes
from whatever list claimed the chat, and a chat no list claims has nothing to
say about notifying, so it arrives in the view silenced. `notify_p` on the
folder is the separate lever, and the two are independent on purpose.

**It overrides the archive here too**, and the reason it looked as though it
could not is worth recording, because the reasoning was wrong in an instructive
way. Hiding on Android works by leaving rows out of the list `DialogsActivity`
builds, and the argument was that an archived chat could therefore never be
pulled in without inventing a row. It does not: `getDialogs(1)` hands back the
account's own `TLRPC.Dialog` objects, and `PurpleGate.filter()` already returns
a fresh list whenever it hides anything, so pulling one in is an append and a
re-sort. Worse, half of it had already shipped by accident - `shown()` answers
for an archived chat without ever asking where it is filed, so such a chat was
already counting toward the badge while being unable to appear in the list.

The two real complications are ordering, not invariants:

- The main list arrives already sorted, so a pulled-in chat has to merge by
  date rather than land at the end.
- An archived chat's `pinned` flag is a pin *inside the Archive*. Letting it
  sort into the main list's pinned run would put it above chats the user
  actually pinned, so the merge comparator counts only `pinned && folder_id ==
  0` as a pin, and `getPinnedCount()` ends the divider's run at the first row
  from another folder.

The chats stay archived - they are still in the Archive row's own list, which is
exactly what a real Telegram folder does with archived chats unless it sets
`NoArchived`. Android needs none of the unread arithmetic the desktop needed
here, because its counters are built from `allDialogs` and from storage rather
than by summing the list on screen.

`settings.toml` is reloaded when it changes on disk, so an edit lands without a
restart. `PurpleWatcher` is `purple_config.cpp`'s `QFileSystemWatcher` in
Android's shape, with two differences that matter:

- **It watches the directory, not the file.** An import - and most editors -
  replace `settings.toml` through a temp file and a rename rather than rewriting
  it in place, and a watch on the old inode goes deaf the moment that happens,
  without saying so.
- **Deletion counts as a change.** That is what takes the "running from the last
  good copy" warning down once the real file comes back, and what puts it up the
  moment the file goes away, rather than at the next restart.

Events are coalesced on a short timer, because one save is several of them and
reloading on each would parse a half-written file and report a syntax error
nobody made. An import reloads twice as a result - once for its own write and
once for the watch - which is idempotent, and the log line says which is which.

The badge and notifications are done, and were verified against a real inbound
message rather than by reading the code: with the chat hidden the gate logged
`value is false`, posted no notification and left the badge untouched; under
Normal the same chat logged `value is true`, posted one and moved the badge to
2. One rough edge stays. With "Include muted chats" turned on, the in-app "All
chats" tab counter still includes hidden chats, while the launcher badge does
not - that counter is computed in `MessagesStorage` from a SQL cursor rather
than from dialog objects, so excluding them there needs a gate entry point that
works without a dialog in hand and still honours mention-gating. In the default
configuration it is already right, so this is left as a known limit.

## Not yet implemented

Nothing outstanding here at the moment.

## Cloud unread counts

`Dialogs::MainList` keeps a second total, `_cloudUnreadState`, from what the
server reports for chats that have not loaded yet. It is not gated, so a badge
can briefly count hidden chats between launch and the dialog list arriving. It
resolves itself as soon as the entries load.

## Implementation

    Telegram/ThirdParty/purple_core/purple/   the submodule, shared verbatim
        purple_settings.{h,cpp}   the model and parser
        purple_engine.{h,cpp}     resolution, pure data
        purple_state.{h,cpp}      state.toml, the cache
        purple_splice.{h,cpp}     the surgical writes
    Telegram/SourceFiles/purple/purple_config.{h,cpp}     file IO and watcher
    Telegram/SourceFiles/purple/purple_focus.{h,cpp}      OS focus sync
    Telegram/SourceFiles/purple/purple_gate.{h,cpp}       the seam to tdesktop
    Telegram/SourceFiles/purple/purple_list_menu.{h,cpp}  list membership menu
    Telegram/SourceFiles/purple/purple_peek.{h,cpp}       the peek hotkey
    Telegram/SourceFiles/purple/purple_preset_box.{h,cpp} the preset picker
    Telegram/SourceFiles/purple/purple_schedule.{h,cpp}   the schedule clock

The four in the submodule have no tdesktop dependency at all - which is what
lets the Android app compile them verbatim. The engine never sees a
`PeerData`, only an id and a `ChatKind`, for the same reason the parser does
not: every policy here is a rule about data, and rules about data are far easier
to prove outside a running app. `purple/test_config.sh` compiles those four
standalone and runs the whole acceptance suite against them in about a second -
which is also a constraint, since anything tdesktop-shaped that creeps into one
of them breaks the harness.

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

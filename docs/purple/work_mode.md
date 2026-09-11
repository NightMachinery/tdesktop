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

Android does the same and reaches further with it: there the value is written
only when the row is actually swiped, so an install that never touched it has
no stored value at all and picks the new default up on upgrade, while anyone
who did swipe keeps what they chose. The default is
`PurpleDefaults.ARCHIVE_HIDDEN`; see [defaults.md](defaults.md) for the rest of
what this fork decides differently before anybody configures anything.

A preset can go further and take the archive away entirely while it runs -
`hide_archive_p`, on by default, documented in
[config.md](config.md#schema). That is a preset's decision rather than a
default, it is only asked while one is filtering, and on the desktop nothing
consumes it yet: there is no pull gesture here and the row is in the main menu
already.

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

### The strips the app fills for you

    [suggestions]
    hide_invisible_p = true

`hide_everywhere_p` above is about what you can *reach*. This is about what the
app *offers*, which is a smaller thing and a far more common thing to want: a
chat you can still search for by name, still forward to and still reach with
Ctrl+Tab, but that the app stops putting in front of you while a preset runs.
It is global rather than per preset, beside `[peek]` and `[recent]`, because it
is a decision about those strips and not about what any one preset lets
through.

The test is "visible anywhere in the profile", and it is deliberately not the
test the chat list uses. `Purple::HiddenFromSuggestions()` asks the preset alone
- `History::purpleHiddenByPreset()`, with the close buffer, the "until"
decisions and the peek all left out of it - and then asks
`History::purpleReachableElsewhere()`, which is the same walk
`any_open_chat_except_in_folder` uses: an extra view of the preset's, or a
folder whose tab is on the strip. A chat that is one click away somewhere was
never hidden. A chat you closed two minutes ago, on the other hand, is not a
chat the preset lets through - it is one on a clock, and a strip that gained
and lost a row as that clock ran would be noise rather than information. A
strip is a standing list, not a view you are looking at, and it should not move
on a timer.

On the desktop the strips are the search panel's frequent-contacts row and its
Recent list, the quick-share popup a message's share button opens, and the
frequent contacts offered when you choose who to send a gift to. The two in the
panel re-push on `Purple::ActiveChanges()`, exactly as the stories strip does
and for exactly the same reason: nothing about a top peer changes when a preset
does, so without it the panel would sit there showing what the last preset let
through until the account itself moved.

On Android it is the same predicate over more surfaces, because more of the OS
asks: the People strip in search, the recent searches, the share sheet's hints
row and the quick-share overlay, the contact pickers a boost or a gift opens,
and - the one that leaves the app entirely - the **direct-share targets**, which
are the conversation shortcuts the system share sheet and a long-press on the
launcher icon or a notification put in front of you. Those are published to the
OS rather than drawn, so they are republished on every reload; a preset that
hides a chat and left a shortcut to it on the launcher would have hidden
nothing.

The **Channels tab of global search** is two strips stacked, and they follow
different rules because they are different claims.

"My channels" is your own channels, so it is a strip like the rest and takes
`hide_invisible_p`: a channel the preset hides is not offered here either. The
list is rebuilt on `Purple::ActiveChanges()` rather than filtered once, exactly
as the Recent row is, because a preset change can put a channel back as easily
as take one away - a filtered copy could only ever lose rows.

"Similar channels" underneath it is not your chats at all: the server picks it.
That makes it the one suggestion no per-chat test can express, so it is a
switch instead - `[suggestions] recommended_channels_p`, off by default, and
asked under every preset including Normal. While a preset filters and the key
says no, the section is empty and the request is never even sent: asking the
server which channels resemble the ones you read is not a question a work
preset should be putting on your behalf. Turning the key on brings the strip
back everywhere, which is stock behaviour.

Untouched on both, and this is the line the key promises not to cross: typed
search, the forward picker, the share box and the Ctrl+Tab switcher. Somewhere
you went looking for a chat by name is not somewhere the app is suggesting one.

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

### How long, and which gesture

    [peek]
    tap           = "5m"
    hotkey_length = "2m"

The two ways of starting one are not the same gesture. A tap on the control is a
decision made while looking at the box it is in; the key is fired mid-sentence,
one hand, to check one thing. `tap` and `hotkey_length` give them their own
lengths, each falling back to `auto_off` when it is not written - so one key
still governs both for anybody who does not care about the difference. `"off"`
from either is a real answer, a peek with no clock on it, and that is why the
fallback is a function in the core rather than a `.value_or(0)` at each place
that reads them.

### The second press extends

Pressing the key while a peek is running adds another `hotkey_length` to it,
measured from the deadline it already has rather than from now - so two quick
presses buy two lengths and not one and a bit - and the toast says where that
landed: `Peeking - extended to 7:00 left`.

The old second press ended the peek, which made the key useless for the thing
the second press nearly always means. A peek is running because something is
still being looked at, and the only way to buy two more minutes was to end it
and start it again: nothing new revealed, every chat list rebuilt twice for it.

It stops at an hour. Past that it is not a peek any more, it is the preset off,
and there is a plainer way to say that than pressing a key twelve times. The
press that finds the cap already spent ends the peek instead, and says so -
`Peek over - it was already as long as a peek gets` - and so does the press that
finds a peek with no clock on it, which has no deadline for an extension to move.
A key that can start something it cannot stop would be worse than a key that
means two things.

### Chips, not a checkbox alone

The preset box carries the lengths as a row of chips under the checkbox: one
minute to an hour, and `until I stop` one position past the end of them. The
checkbox is still the on/off - tapping it starts a peek of `tap` - and the chips
are the same switch with a number on it.

Tapping a chip while a peek is running **restarts** it at that length rather
than adding to it. A chip that says `5 min` and leaves you with eleven is a chip
lying about what it did; adding is the key's job, where there is no number on
screen to contradict.

The lit chip follows what is *left*, not what was asked for, so a five-minute
peek with ninety seconds on it lights `2 min`. Nothing anywhere remembers the
length a peek was started with - `state.toml` holds a deadline and that is all -
and inventing a memory for it so that a highlight could sit still would be
storing a fact to make a picture tidier.

The row itself is `PeekDetentsSeconds()` in the core, and so is the rounding
that picks the lit one. Two hand-written lists is exactly how a phone's chips
and a desktop's row come to offer different minutes for the same feature, and a
list of lengths is the kind of thing somebody edits on one side only.

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

The main menu entry reads `Work Mode: work (peeking, 4:12 left)` while one is
running - the word alone raised the question of how long and did not answer it -
and it drops back to `(peeking)` for a peek with no clock. That second hand
ticks only while there is a countdown to move: the menu stays open for as long
as somebody leaves it open, and a label rewritten once a second for a preset
that is not going anywhere would be a timer running for nothing.

The preset box carries a checkbox naming the key: a hotkey with no visible
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

Among the rules covering a moment the **narrowest window wins**. A day of
`08:00-17:00 work` with `12:00-14:00 lunch` cut out of it gives lunch at one
o'clock whichever of the two was written first. Windows nest because that is
what people write them for - a long stretch with something taken out of the
middle - and the rule that reads as the exception has to beat the one it is an
exception to, or writing it down does nothing.

A window that crosses midnight is measured the long way round, through
midnight, so `22:00-06:00` is eight hours and not sixteen. Two windows of the
same length covering one moment keep the order they were merged in: ruleset
specificity first, then file position. That is the tie-break rather than the
rule, because two equally wide windows over one minute is a file saying two
things about it, and position is the only answer that can be predicted by
reading.

Leaving the inner window is a move to the **outer** rule's preset rather than
to `outside`, and it counts as a window starting - so work comes back at two
o'clock even if a preset was chosen by hand over lunch. Leaving the outer one
is a window ending, which is the asymmetry two sections below.

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
passed, which is no reason to undo something asked for, so the preset between
windows is applied only when the preset in force is one the schedule itself put
there. The asymmetry is the point, and it is why `state.toml` records what put
the current preset in place rather than only what it is.

Focus is left alone in both directions. It is the more immediate signal, and a
schedule fighting it would leave neither of them predictable.

### The preset between windows

    [schedule]
    outside = "home"

`outside` is what the schedule wants whenever no rule covers the moment. It used
to be a constant - Normal - and it is a key because not every day has stock
Telegram at its edges: if your default is Home, five o'clock should put Home
back rather than hand you the unfiltered account. A name no preset backs warns
and falls back to `normal`, the same rule a rule's own `preset` follows.

It does not make the schedule pushier, and the paragraph above is why. A window
ending is now a move to `outside` rather than a move to Normal, and it is still
a window ending: it lands only when the schedule was what put the running preset
there. Five o'clock aiming at Home does not overrule a preset you chose at four.

That is the one part of this easy to get subtly wrong twice over. "The target is
Normal" was a serviceable stand-in for "a window is ending" right up until the
day it stopped being one, and there are two ticks that would each have to be
fixed. So the test is a single function in the shared core, `ScheduleApplies`,
and both clients ask it rather than spelling it out.

### One file, many devices

A phone and a laptop want different schedules and the same `settings.toml`. The
file is what travels - Saved Messages carries it, see [sync.md](sync.md) - so
the answer is for the file to describe the devices, rather than for each device
to keep a copy of its own to drift out of step with the others.

    [[schedule.rulesets]]
    name   = "phone"
    device = "mobile"
    mode   = "enabled"

    [[schedule.rulesets.rules]]
    days   = ["mon", "tue", "wed", "thu", "fri"]
    from   = "09:00"
    to     = "17:00"
    preset = "work"

A ruleset is a named group of rules plus the answer to "which devices is this
for". The tier is per ruleset rather than per rule, because "for the phone" is a
property of a group of rules written together, and per-rule targeting would mean
repeating the device on every line of a block that is plainly one block.

**Which rulesets run.** A ruleset applies to a device when its `device` matches
and its `mode` is not `disabled`. Among the applicable `enabled` ones only the
most specific tier runs: a ruleset naming this device's id beats one naming its
platform, which beats one naming its class, which beats `any`. Every applicable
`always` ruleset runs as well, whatever won there.

Replacing rather than layering is the whole point. A ruleset written for this
one laptop *replaces* the desktop one instead of piling on top of it, which is
what makes "one file everywhere, refined per device" work at all - the
refinement is a substitution, so you never have to reason about what the general
rules would still have been doing underneath it. `always` is the escape hatch
for the rules that really are true everywhere - never during the night - so they
do not have to be copied into each device's ruleset and kept in step by hand.

Specificity is a property of what the ruleset asked for and of nothing else, so
two devices always agree on which of two rulesets is the more specific. That is
what keeps the answer readable from the file alone, on a machine you are not
holding.

**Merge order.** The chosen rulesets' enabled rules are concatenated, the most
specific ruleset's first and file order among equals. Everything downstream sees
one flat list of rules and one `outside`, so first-match-wins is exactly what it
was before rulesets existed - which is also how the flat `[[schedule.rules]]`
array keeps working: the parser reads it as an implicit ruleset, device `any`,
mode `enabled`, placed first, and it resolves through the same path as the rest.

**The `outside` between them.** The most specific chosen ruleset that names one
wins; when none does, `[schedule] outside`. A ruleset that leaves the key out is
not saying `normal`, it is saying nothing, and the question goes up a level.

**The device id is the operating system's.** Android's `ANDROID_ID`, macOS's
`IOPlatformUUID`, the Windows `MachineGuid`, `/etc/machine-id` on Linux. Each is
hashed with SHA-256 and cut to its first four bytes, and what a ruleset sees is
`macos-3f9a2c1d` - the platform, a dash, eight hex characters.

Two reasons for the hash rather than the identifier itself. `settings.toml` is a
file people mail to themselves and paste into bug reports, and the raw value is
the one the rest of the system uses to mean this machine, so it never leaves the
function that reads it. And eight characters is short enough to type into a
ruleset by hand, which is how it gets there.

The OS's identifier rather than one the app makes up, because it survives what
the app can lose: a wiped data directory, a reinstall, a `settings.toml` deleted
by hand. An id that changed every time you reinstalled would need its ruleset
rewritten with it, and hand-editing per device is the thing rulesets exist to
remove.

A device that will not say - no `/etc/machine-id`, a platform that refuses -
reports no id at all. That is a legitimate answer and is treated as one: it
matches the rulesets that asked for no device in particular, and skips every
ruleset naming an id. Nothing is invented to fill the gap. An invented id would
be another file in the config directory and would still be lost by the very
reset it was meant to survive.

**Labels.** `[devices]` maps an id to a name:

    [devices]
    "macos-3f9a2c1d" = "the laptop"

An id nobody named shows as itself, which is also where you get the id to type
in the first place. The labels live in `settings.toml` rather than on each
machine so that they travel with the file that uses them.

### Pausing

`schedule_paused` holds it off entirely, and the preset box carries the switch.
Nothing in `settings.toml` turns it on, because it is a decision about today
rather than about the configuration. The row is there only when the file
describes a schedule at all - a switch that holds off nothing explains nothing.
"At all" counts any ruleset, including one written for another device: a row
that vanished from the laptop while you were writing the phone's schedule would
read as the file having broken.

Unpausing catches up with wherever the schedule has got to, by the same boundary
rule: the target moved while it was not looking.

`schedule_paused_until` gives the pause a deadline rather than leaving it open.
The tick treats a pause whose moment has passed as unpaused, clears both fields,
and then runs the ordinary boundary rule in the same pass - so the windows that
opened and closed while it was paused are caught up on once, immediately, rather
than at the next window edge, which could be a day away. Zero, which is what an
older `state.toml` says because the key did not exist, means what a pause has
always meant: until you lift it.

It is a moment rather than a countdown, for the same reason as the peek
deadline: a pause is measured in hours or days, so a pause that ran out while
the app was closed has already expired by the time anything reads it again.

Under the switch, the box says what is being held off - `Schedule: work until
17:00, then home` inside a window, `Schedule: home until 09:00` between them,
`Schedule paused until` a date while it is paused. The file was the only place
the windows were ever written down, and reading a list of times to work out
which one is running now is exactly the arithmetic a screen should be doing for
you. It re-reads the clock every thirty seconds while the box is open, the
schedule's own resolution, so it is never more wrong than the schedule is.

A second, dimmer line under it says which machine that is about - `(this device:
the laptop)`. That question did not exist until one file could describe several
devices, and it has to be answerable from here: a rule that runs on the phone
and not on this laptop otherwise looks like a rule that does not work. It is
also where the id to type into a ruleset comes from, on a device the file has
not named yet.

Every branch of that line says which state the file is in rather than falling
back to a sentence that would be a lie in it. A schedule switched off in the
file says so, and one whose every ruleset is for some other device says that -
neither is allowed to read as `home until 09:00`.

### Editing it from the app

Settings > Advanced > Purple > Schedule opens the whole thing: the two
switches, the preset between windows, and one row per ruleset - `phone · Any
phone · On · 2 windows` - with the flat `[[schedule.rules]]` array shown as a
ruleset called **Rules**, since that is what the parser makes of it. Tapping a
ruleset opens its mode, its scope, its own `outside`, and its windows; tapping a
window opens the seven days, the two times, the preset and the enabled switch,
with Delete on one that already exists.

**It writes the file, one key at a time.** Every edit goes through the core's
splice ops - the same ones the phone uses - so a `settings.toml` full of
comments and hand-chosen spacing comes back out of an edit still full of them.
The file remains the thing that decides; this is one more way of writing it, not
a second place the schedule lives. There is nothing here you could not have
typed, and nothing you typed that this cannot show.

**An edit lands on the rule you were looking at, or not at all.** A rule has no
name, so it is addressed by its raw position, and the box sends the window and
preset it read off that rule along with the write. If the rule at that position
no longer says the same thing - you edited the file in a text editor while the
box was open - the core refuses, the box says so in the core's own words, and
closes. The list behind it has already rebuilt itself from what the file now
says, so the honest next step is to look at that rather than to keep editing a
copy that has gone stale.

The times are text fields spelling `09:00`, validated with the same
`ParseTimeOfDay()` the parser uses, rather than a clock widget. A picker would
be a second notion of what a time is, and the one case where the two disagreed
would be the one that mattered. The box refuses a window with no length and one
with no days before writing, so those two come back as a sentence rather than
as a refusal explaining itself after the fact.

**Rules the parser threw away are listed, not hidden.** A broken rule never
reaches the app as a rule - all that survives is the warning, `schedule rule 3:
'from' and 'to' are the same time, skipping it` - so the box shows those
warnings under the windows of the ruleset they belong to. Without that, a rule
you wrote and cannot see would look like a rule the box had eaten.

Every write is followed by a rebuild whether or not it landed, so a switch ends
up where the *file* is rather than where the click left it. Changes made from
outside rebuild it too.

Beside the Schedule row, **This device** shows the id this machine reports for
itself and lets you name it. The name goes into `[devices]` in `settings.toml`
rather than into anything local, so it travels with the file that uses it and
every machine reading that file calls this one the same thing.

### The tick

Thirty seconds, which is therefore how late a boundary can be. Computing the
exact moment of the next one and sleeping until it would be tidier, and would
then have to survive every way a wall clock can move underneath it - a laptop
waking, a timezone change, the DST hour. Re-reading the clock on a cheap tick
survives all of them by construction.

A settings or state change re-ticks immediately, since either can change the
answer sooner than the next thirty seconds would.

## Sending the file after every save

`[sync] send_after_save_p`, off by default, posts `settings.toml` to Saved
Messages whenever the app itself writes it - a switch, a list edit, a rule -
debounced five seconds so a run of taps is one document. It is the same upload
the manual **Send settings to Saved Messages** performs, with the confirmation
box left out: the confirmation was given once, in words, when the switch was
turned on.

Only the app's own writes go through it. There is exactly one function in
`Purple::Config` that writes `settings.toml`, and the hook is on it, which is
why a switch added next year gets this for free. An edit made in a text editor
arrives through the file watcher instead and is deliberately not this.

**It never sends after an import, and never sends bytes it has already sent or
received.** `state.toml` keeps two fingerprints - length and SHA-256 - one for
the last file this machine sent and one for the last it wrote because another
machine sent it. The second is what stops the ping-pong: A saves and sends, B
imports and writes, and without that fingerprint B would then send back what it
had just been given, A would import it, and the file would bounce between two
machines that already agree.

The rule is `ShouldAutoSend()` in the core, pure and tested there, taking the
settings, the state, the bytes and whether this write is an import. Asked twice
- when the write happens, and again five seconds later before anything is
posted - because in between, the switch can have been turned off, the file put
back to what it was, or an import can have landed the very bytes that were about
to be offered to the machine that sent them.

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

One exception to putting the preset back, and it is what a long focus session
needs. A schedule boundary that passed while focus held the preset was recorded
by the tick and never applied, because focus is the more immediate signal.
Restoring the pre-focus preset would then leave the tick nothing to do - the
target it compares against has already moved - and the window would be missed
until the next boundary, which for an evening session is the next morning. So
leaving runs the boundary rule itself, on the pre-focus source: a window that
has opened takes over, and one that has closed only undoes a preset the
schedule itself set. It needs to know what the schedule wanted when the session
began, which is a note rather than a decision and so is kept beside
`state.toml` rather than in it - in memory on the desktop, in a preference file
on Android. After a restart there is no note, and leaving restores exactly as it
did before this rule existed.

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

## Last seen

Telegram will not always say when somebody was last seen. It does say *why*,
and the fork passes that on rather than leaving "last seen recently" standing
there as the whole answer.

A coarse status is one of three things, and the server tells the two apart that
can be told apart:

- **Coarse because of your own privacy.** You hide your last seen from them, so
  Telegram hides theirs from you - its reciprocity rule. The status carries a
  `by_me` flag saying exactly that, and the fork appends `share yours to see`
  to the line, after the same middle dot every other status suffix uses. It is
  the one case with something to do about it.
- **Coarse because of theirs.** The same words with no flag. Their setting,
  nothing to offer, so nothing is added.
- **"a long time ago"** - `userStatusEmpty`. Nothing is added, and that is the
  considered answer rather than a gap. An abandoned account and an account that
  blocked you look identical here, and there is no field that says which. The
  fork does not guess at a block: it is the one thing here that would be
  unforgivable to be wrong about, and being right about it half the time is not
  a feature.

`[last_seen] reasons_p = false` takes the whole tail away and leaves the status
line as upstream writes it.

Where the line is short of room - the chat header, on a narrow window - the
words collapse to an eyes mark. The choice is made against the width the header
actually has, not against a guess: a sentence the header would only elide into
nothing is worse than the mark it had room for. The profile has room and always
gets the words.

### Where the line is drawn

Six places in the app write a last seen, and they do not all have the same room
or the same click. The chat header and the profile carry the words *and* the
link: the tail is what opens the sheet, and both have somewhere to put a second
click that is not already spoken for. Member lists, the contacts box,
add-participants, the forward and share pickers, the chat preview popup, the
short info box and the participant editor carry the mark alone - `· 👀` after
the status - which says the fork has something to add about this last seen and
leaves the two places above to be where you act on it.

Informational on purpose, and not for want of trying. A `PeerListRow` has
exactly one click and it belongs to the row: opening that person, or ticking
them. The status is painted as one elided string with no per-region hit test
anywhere in the class, so a link there would mean inventing a seam rather than
using one, and a row that sometimes opens a sheet and sometimes opens the
person is worse than a row that always does the one thing.

A remembered read replaces the coarse phrase in all six, not only in the two
that can trade. What a trade bought is a fact about that person, and a member
list still saying `last seen recently` beside a profile saying `last seen
14:32` would be the fork disagreeing with itself in two windows of the same
app.

Rows have no status subscription of their own - the list re-derives a status
when the time it was given runs out - so a row carrying the fork's tail asks
for at most a minute of that time. The remembered line ages inside its own
words (`as of 3 min ago`), and a minute is the resolution those words have.

### Show mine to see theirs

The tail is a link. It opens a sheet that explains the moment of exposure and
offers to make the trade once:

1. Your current last-seen privacy rules are fetched.
2. That one person is added to the allowed exceptions - and taken out of the
   disallowed ones, if that is where they were.
3. Their status is asked for, and the answer waited on for `trade_hold`.
4. Your rules are put back exactly as they were. Always: on a read, on a
   timeout, and on an error.

They are not told. Nothing else about your privacy changes, and no other
person's view of you moves for those few seconds - the rule that changed names
them and nobody else.

It can come back with nothing, and that is a real answer rather than a failure:
if they hide their last seen for their own reasons, showing them yours buys
nothing. The trade is still written down, because the cooldown counts attempts
rather than successes.

`trade_p = false` takes the offer away and leaves the explanation. That is also
what the sheet's "Don't offer this again" checkbox writes, rather than a second
flag somewhere meaning the same thing - the switch already exists, it is in
Settings > Advanced > Purple, and it is in the file you can read.

Upstream's own one-tap offer is gone on both clients. Telegram puts a small
button beside a coarse last seen - `when?` on the desktop - and confirming it
saves an empty last-seen rule, which means *everybody*, permanently, with
nothing anywhere in the app to put it back. That button opens the trade now,
and with `trade_p` off it is not drawn at all rather than falling back to what
it used to do. Leaving it standing beside the trade would have kept the
footgun and merely parked a safer path next to it; anybody who does want to be
visible to everybody can still say so in Settings > Privacy, on a screen that
can say the opposite again tomorrow.

### The memory, and the cooldown

A read is remembered for `trade_remember` (a day, by default) and shown in
place of the coarse phrase: `last seen 14:32 · as of 3 min ago`. Both halves
are needed. The time is what you traded for; the age is what stops it reading
as live. Past the window the record is dropped rather than shown as older and
older news, and the line falls back to the coarse phrase and its tail.

The records live in `state.toml`, one per person - a second trade replaces the
first - and they never leave the machine. Settings > Advanced > Purple > Trades
lists them.

One trade per person per `trade_cooldown`. A trade is a moment of exposure
chosen on purpose; one offered again every time their chat opens would be a
standing subscription nobody agreed to.

The remembered line is a link too, and that is a repair rather than a flourish.
The tail used to be the only door into the sheet, so the first trade replaced
the door with the read and the second trade was unreachable for a whole
`trade_remember` - a day, by default - unless the record happened to expire
first. The remembered line now carries a `· refresh` of its own, and the same
eyes mark where there is no room for the word, and it opens the same sheet. It
stops being a link when their last seen is no longer coarse because of *your*
rules: they have changed their own privacy since, and there is nothing left to
trade for.

Inside the cooldown that sheet opens rather than refusing. It says `You can
refresh in 3:12`, counting down every second from the read already written
down, with the button held disabled and greyed beside it; when the wait runs
out the line becomes `You can refresh now` and the button - `Refresh now`
rather than `Share once`, because this is a second look at somebody already
traded with - becomes pressable without the box having to be closed and opened
again. A wait is not a refusal, and a toast that fires and vanishes cannot say
how much of one is left.

The number is recomputed from the clock on every tick rather than decremented,
so a box left open across a suspend does not go on counting a wait that
wall-clock time has already spent.

The other refusals are still toasts, because none of them is a wait: the offer
switched off, a last seen that is not coarse because of your own rules, and a
trade already running are each a "no" that will not turn into a "yes" while the
box sits there.

The profile's button agrees with the line beside it. It is drawn exactly when
the status line is tappable - one answer, asked of the core - so it stands for
a remembered read as well, and it goes when `trade_p` does. It no longer hides
for a premium account: upstream's button was a promo, this one is not, and the
fork's own local premium was hiding the trade from precisely the people who had
gone looking for the fork's features.

### What each side does

The core owns the rule (`ReasonFor`, on the three facts a status carries), the
memory (`RememberTrade`, `RememberedTrade`, `TradeAllowed`) and the keys.
Both apps ask it the same questions.

The privacy calls are per client, because the API layer is. On the desktop the
fetch, the save and the restore all go through
`Api::UserPrivacy` - `reload(Key::LastSeen)`, `value(Key::LastSeen)` and
`save(Key::LastSeen, rules)`, the same three calls the Privacy and Security
screen makes - and the status is asked for with `users.getUsers`. The `by_me`
flag arrives as `Data::LastseenStatus::isHiddenByMe()`, which upstream already
reads for its own "Show my Last Seen" button, so this is the same signal used
for the same purpose rather than a second interpretation of it.

One consequence of going through that layer: the rules are put back as the
round trip understood them. `Api::UserPrivacy` reduces the server's rules to
allowed and disallowed peers plus an option, and anything it cannot represent
would not survive - the same reduction the Privacy screen's own save does. The
chats a rule names come back in the same response that carries the rule, so
they are loaded by the time it is read.

Only one trade runs at a time, for the whole app. Two would be two windows of
exposure that were agreed to once.

Each step is logged: reading our rules, showing ours, the read or the timeout,
and the rules going back.

## Screen time

How long the app has had you, out of a log this machine keeps and never sends.
Off until `[screen_time] enabled_p` says otherwise: it is a record of what you
looked at and for how long, and nothing should start keeping one of those
because a version number moved. The desktop records and draws it; Android does
neither yet.

### What is recorded

One line per event, and nothing else. No totals, no sessions, no days - every
threshold in `[screen_time]` is applied when the log is read rather than when
it is written, which is the whole reason the file holds events: changing
`action_span` or `idle_after` re-derives the history you already have instead
of only affecting tomorrow. A log of totals would have baked yesterday's
settings into yesterday forever.

The events are deliberately few. Anything the core can work out for itself -
which chat a session belongs to, how long it ran, whether it was active - is
not one.

- `open` and `close`: a chat came to the front, and went away with the app
  still in front. The `open` carries the chat, its kind, the running preset,
  and whether the preset hides it.
- `foreground` and `background`: the app arrived and left. Background ends
  whatever session was running, because a chat you cannot see is not screen
  time.
- `preset`: the running preset changed, which cuts the session so that every
  second of it has exactly one preset.
- `action`: something you did in the composer - a burst of typing, a send,
  voice recording starting, a file chosen, a reply or an edit begun.
- `idle` and `resume`: input stopped for `idle_after`, and started again.

Time that is not in a chat - the chat list, search, settings - is recorded as a
session with no chat and the kind `elsewhere`, so the splits add up to
foreground time rather than to something smaller with no name.

### The active rule

Actions are the signal, and everything else in a session is reading.

Each action counts as active for `action_span` from where it lands, which is
why a burst of typing is one event and not one per keystroke. When the next
action lands within `active_gap` of it, the whole gap between them counts as
well - that is what makes a conversation read as active time rather than as a
row of three-second spikes. A lone action counts only its span, and the total
is clipped to the session it is inside: send and close instantly and you were
active for the moment you were there, not for three seconds afterwards.

Idle pauses the session rather than ending it, and the pause is stamped back to
where input actually stopped rather than to the moment the watchdog noticed.
Time spent paused is subtracted from the session, which is what makes the
totals add up to time actually spent looking.

### The log

`screentime.log`, beside `settings.toml` in the config directory. Append-only,
tab-separated, seven fields:

    unix_ms  kind  dialog_id  chat_kind  preset  action  hidden

Tabs rather than commas because a preset name is whatever you typed and a comma
in one is likelier than a tab. A line that cannot be read is skipped in
silence: the file is append-only and written from several places, so a
truncated last line after a crash is expected rather than exceptional, and one
lost event is worth far less than the rest of the history.

It never leaves the machine. Nothing in the fork reads it for sending, and
nothing will: two devices would double-count nothing useful, and this is a
record of what you looked at. It is pruned to `retention_days` when the app
starts and every six hours after that, and a pass that finds nothing to drop
reads the file and writes nothing.

### What the desktop hooks

- **The chat in front** is `Window::SessionController::activeChatValue()`, one
  subscription per window. Whichever window last changed its active chat is the
  one the log follows, so a second window open on a second chat is counted as
  one chat at a time rather than two.
- **Foreground and background** is `Core::App().appDeactivatedValue()`, which
  also covers the screen locking on the platforms that deactivate the app for
  it. There is no separate lock signal on the desktop - `screenIsLocked()` is a
  flag with nothing to subscribe to - so a platform that locks without
  deactivating would be counted as still in front.
- **The preset** is `Purple::ActiveChanges()`, so a preset moved by the
  schedule or by a focus mode cuts the session exactly as one chosen by hand
  does.
- **Actions** are `HistoryWidget` and `ComposeControls`: the field's changes
  (throttled to one event per `action_span`, in the recorder rather than at the
  call site), the send stream, the voice recorder starting, a file actually
  chosen rather than the picker opening, and a reply or an edit begun.
- **Idle** is `Core::App().lastNonIdleTime()`, checked every five seconds. That
  is the app's own idle clock - key presses, wheel, mouse and touch anywhere in
  the app, plus the system's last input time - which is wider than the history
  and the composer, and narrower than it looks: the app leaving the front ends
  the session anyway.
- **Hidden while peeking** is `Filtering()` and `History::purpleHiddenByPreset()`
  at the moment the chat opens. That predicate is the preset's verdict with the
  peek taken out, which is exactly the question: during a peek nothing is
  hidden, and the number wanted is what would have been.

Everything above returns immediately while `enabled_p` is false. No file is
opened, no timer runs, and no hook does anything.

### The box

Settings -> Advanced -> Purple -> Screen time. The row itself carries the
digest - "This week: 6 h 12 m, 41 % active, top: Alice" - which is the whole
feature on most days: a line that answers the question without opening
anything.

Inside: the switch that turns recording on, a period (Today, this week, this
month, or a custom range picked as two dates), a headline with the total, the
active share and the change against the period before it, and a bar chart -
hours for a day, days for anything longer - stacked by chat kind and painted
with the palette's userpic colours, so a theme that repaints the app repaints
the chart.

Then the filters: "Active only", which switches which of a session's two clocks
every number on the screen reads rather than dropping the reading sessions; one
switch per kind; and a preset, with Normal offered by name because the log
spells it as the empty preset.

Then the chats, ranked longest first, each with its userpic, a bar proportional
to the longest, and its own active share. Clicking one opens its own page: day
by day, and when in the day. Then "reading load" - every day in the period
folded onto one clock, which is what a schedule window is placed by - and for a
month, an hour-by-weekday heat map. Then "hidden while peeking" as its own
number, the budgets - which are added and edited here, not only listed - and an
export.

The export is CSV through the save dialog, and it writes sessions rather than
buckets: a bucket is one way of looking at the log and a session is what the
log actually says, so an export anybody can re-bucket is worth more than a
picture of this box's choices.

Nothing here is stored in the shape it is drawn in. Every number comes out of
the raw log through the core when you look, so an edit to `settings.toml` while
the box is open redraws it.

### Budgets and the cover

A budget is an `[[screen_time.budgets]]` entry: a target (`all`, `chat:<id>`,
`kind:<kind>` or `preset:<name>`), a `per_day`, and a `mode`. The day's ledger
is derived from the raw events like everything else, so a changed threshold
applies to today's total and not only to tomorrow's.

The box lists them with what they have spent against what they allow, and
writes them. "Add a budget" under the list opens an editor: the target as four
radio rows - everything, one chat, one kind of chat, the time under one preset
- with the app's usual chat picker behind the chat row, the app's presets
behind the preset row, and Normal offered by name; hours and minutes for the
allowance; soft or hard; and, for a hard cap only, how long a snooze lasts and
how many there are in a day. A budget's own row opens the same editor on it,
with Delete beside Save. A chat's own screen time page carries the editor too,
opened on that chat, which is the one place "this chat" means something without
going through the picker.

The writes are the core's own splice ops, the shape the schedule's rule editor
already had: appended after the last budget, edited in place key by key,
addressed by position in the raw array - the budgets the parser threw away
counted in - and refused unless the budget still at that position says the
target the box was opened on. A refusal is shown in the core's own words rather
than only logged, and the editor closes either way, because on a refusal the
file is not what it was opened on any more.

A write is an edit to `settings.toml` and reloads live, so the list behind the
editor is redrawn from what the file now says rather than from what the editor
thought it did. Hand-editing the file is still the same thing by another route.

A soft budget shows a bulletin once per chat per day when its allowance is
gone. The "once" is remembered in `screentime_notices` beside the log, a line
per bulletin holding the day, the budget's index and the chat, so restarting
the app is not a reason to say it again. It is beside the log rather than in
`state.toml` for the same reason the snooze count is: which chat has already
heard about one afternoon is this client's bookkeeping, not a fact about Work
Mode.

A hard budget puts a cover over everything below the chat's top bar - the
history and the composer with it - naming the budget and its allowance. The
chat stays nameable and closeable, messages and notifications are untouched,
and the session keeps running behind the cover, counted as reading, so time
spent sitting on it still shows in the total. One button offers another
`snooze` minutes, up to `snoozes_per_day` of them; the count is kept in
`screentime_snoozes` beside the log, because `state.toml` is the core's schema
shared with Android and a desktop cover's snooze count is this client's
bookkeeping about one afternoon. `snoozes_per_day = 0` makes the cap absolute
and leaves the button out.

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

That switch back carried a bug worth recording, because the symptom was the
whole chat list going blank and the cause was several layers away from anything
about presets. The fork called `selectFirstTab()` on every reload - and a
reload is not a preset change: a peek, a schedule tick, an "until" expiring and
the watcher's echo of the app's own write all produce one. `selectFirstTab` has
no "already there" guard, so on All chats it still ran the tab animation.
`onPageSelected` then returned early, because the selected type had not moved,
leaving the second page hidden - while every intermediate animation frame went
on translating the first page by `progress * width`. The last frame hit the
guard and returned before the swap that resets that translation, so the list
was parked exactly one screen width off-screen. Switching tabs by hand did a
real swap and put it back, which is why it read as intermittent rather than as
a broken tab.

Three things fix it, and only the last is about Work Mode at all.
`onPageScrolled` now returns whenever the second page is not visible and no
search is running, rather than only on the terminal frame - no page is in
flight, so there is nothing to translate. The call site selects the first tab
only when it is not already selected, mirroring the guard `scrollToFolder`
already had. And "the preset changed" is now the strip's own identity - the
running preset's name, the extra views' names and a digest of the folders -
rather than the gate's generation counter, which moves on every reload of any
kind. Extra-view tab ids are carried across reloads by view index so the
stable-id rescue can match them at all, which is what makes that last change
safe for somebody sitting on a folder tab.

The amplifier is gone too: the watcher now compares the file it was woken for
against the length and digest of the bytes the gate is running on, and skips
when they match. That is the app's own write coming back at it, and suppressing
it there covers every writer - a list edit, an import, the editor - without a
hook in `writeAtomic`, which is shared with `state.toml`. An explicit reload is
never suppressed; only the echo is.

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

One divergence, and it is about where the menu lives rather than what it does:
it is offered for **one chat at a time**, because membership is a property of a
chat and a tick meaning "some of these" would have no honest answer.

There are two ways in. The selection mode's overflow, next to *Add to folder*,
is the verified one. The per-chat preview menu - long-press with preview, the
closer analogue of the desktop's right-click - carries the same entry, but the
software renderer on the test box cannot put a preview up at all, so that entry
point has never been driven; both call the same `PurpleListBox.show()` behind
the same `PurpleListMenu.available()`, so what is untested is the entry rather
than the behaviour. It is written down in `todo.md` rather than claimed here.

The rest is the desktop's, unchanged. Every list is offered, including one that
matches by `kinds`. Nothing appears at all unless a list has been written.
Membership is global, not per preset. And a failed splice - an unwritable file,
or a list written as an inline array it will not edit a line at a time - leaves
the file and the running resolution exactly as they were.

Not ported: the line naming what is currently deciding the chat
(`In 'bots': hidden`). That needs the resolution to say which entry won, which
is a question the bridge cannot answer yet.

### Peek and the schedule

Both are ported, and both are the same shape: the core decides, and Android is
the clock that asks it.

**Peek cost almost nothing, and that is the point of the seam.** `Visible()`
already answers `ShowMode::Always` for every chat while `resolved.peeking` is
set, and Android's `visibleNative` asks it the same question it always did - so
the moment the bridge sets that one flag, every row, every notification decision
and every badge honours the peek without a line of Java. What did not come free
is the folder strip, which a peek puts back whole, and the reorder guard, which
a peek lifts because the strip is the account's own again.

The flag is set in the bridge after the resolution, exactly where the desktop
sets it and for the same reason: `ToCache()` has no field for it, so a peek can
never be persisted into the fallback and come back revealed with nothing left
running to put it back. An expired flag is cleared there too, and rides home in
the state rewrite the load already returns - which is why the Android timer has
no clearing half of its own.

**There is no hotkey, so the checkbox is the control rather than its
affordance.** `[peek] hotkey` and `hotkey_length` are read and ignored on
Android; a phone has no key to bind, and the desktop's own reason for the
checkbox - that a hotkey with no visible affordance is a hotkey nobody
remembers - is the whole of what is left. It sits in the preset picker, next to
the preset it suspends, saying `Peek - nothing is hidden under Normal` when
there is nothing to do, and counting itself down while one is running. A tap
starts a peek of `tap`, which falls back to `auto_off` in the core, including
`"off"`, which reads as *until you turn it off* rather than as a countdown that
never moves.

**The three gestures land differently, because the hotkey is not there to take
one of them.** A tap while a peek is running *extends* it by another `tap`,
where the desktop's checkbox ends it; ending is the **long press**. On the
desktop the checkbox can afford to mean "stop" because the key means "more",
and on a phone there is no key, so the two meanings have to be two gestures on
the one control. The tap gets the extension rather than the stop because a tap
while the chats are back is nearly always "not yet" rather than "done" - the
peek is running because something is still being looked at. It still ends the
peek when there is nothing left to extend, with the same hour cap and the same
two sentences the hotkey uses, for the same reason: a control that can start
something it cannot stop is worse than one that means two things.

**The chips are the desktop's row, laid out for a thumb**: a horizontally
scrolling strip under the checkbox rather than a wrapping block, so the row
keeps its height whatever the core's lengths are, and a tap on one starts or
restarts a peek at that length. Every decision in them is the desktop's,
because all of them are the core's - `PeekDetentsSeconds()` for the lengths,
`PeekDetentIndex()` for which one is lit, `PeekLeftSeconds()` for what it is an
index of, and "until I stop" one position past the last detent. The bridge
carries the row and the tap length in the load JSON (`peekDetents`, `peekTap`,
`peekUntilStopped`) and exposes the rounding as a native of its own, rather than
letting Java round: a second copy of "nearest, and a tie reads as the shorter"
is exactly the kind of thing that comes to differ by one chip on one platform.

**Android has no starter `settings.toml`,** so there is nowhere to put a
`tap = "5m"` that a fresh phone would read. The desktop writes one on first run
and the phone does not: a file arrives by import from Saved Messages or is
typed into the editor, and until then the picker says the file is empty and
names it. A file that says nothing about `tap` gets `auto_off`, and one that
says nothing about either gets a peek with no clock on it - which is the honest
answer to "how long", not a default hidden in the Java.

**The schedule is a pure function in the bridge and a `Handler` in Java.**
`scheduleTickNative()` and the desktop's `Runner::tick()` are two calls to one
core function, `ScheduleStep()` - the same half-open windows, the same
narrowest-window-wins, the same asymmetry where a window starting overrides a
preset chosen by hand and a window ending only undoes one the schedule itself
put there - which returns nothing at all on every tick that is not at a
boundary, which is almost all of them.

They were two implementations until 2026-09-08, written in C++ both times, with
the same explanatory comments copied between them and a test on neither. What
was easy to get subtly wrong was exactly what was duplicated: the order of "lift
the pause, then run the boundary rule", and the asymmetry above. Focus sync went
the same way, into `FocusStep()`, and there the two had already parted company -
the missed-window rule above was only ever in the bridge. So had the status
line, into `ScheduleStatusNow()`, where one client could tell "no rules" from
"none of them are this device's" and the other could not, and each hunted for
the next window its own way. The core says which sentence and hands over its
parts; the wording stays in each app, because Android's is in `strings.xml` and
goes through its translation pipeline.

Thirty seconds, the desktop's interval, for the desktop's reason: re-reading the
clock survives a device waking, a timezone change and the DST hour by
construction, where computing the next boundary and sleeping until it would have
to survive each of them by hand. The tick runs while the process does, which on
this fork is whenever the background connection is up - and that is exactly when
a boundary still has to land, because silencing has to change with no UI on
screen. It stops entirely when the file describes no schedule, or when the
switch in the picker has paused it, so an account with no rules pays nothing.

Two things the tick does *not* do, both inherited. It never writes when only the
clock moved, so a preset chosen by hand stands until the next boundary. And when
the target moved but the rules say leave the preset alone, it records the target
and rebuilds nothing - there is no view change to show, and the write is what
makes that boundary happen once rather than on every tick from then on.

### The "until" decisions, and the line that says why

Both are ported, and they landed together because they share one surface: on
the desktop the line is the header of the chat-list context menu and the three
"until" entries sit under a separator in the same menu. Here that surface is the
list box.

**The overrides ride in the load result, not a query.** They are per-chat state
read on the row-drawing path, so the shape peek established is the one they use:
the bridge prunes what has expired, hands over the running preset's live ones
with everything else, and Java answers from that list with a clock comparison.
Nothing reaches JNI per row. The bridge is also where the pruning happens,
because `load()` is already the one place that rewrites `state.toml` - so an
entry leaves the file at the moment it stops being true rather than at the next
unrelated change. One timer, armed for the earliest deadline, the same as peek's
and for the same reason.

Two hooks, matching the desktop's two exactly: the view (`show` reveals, `hide`
takes away) and the silencing (`notify` lifts the preset's mute and only the
preset's, `hide` adds one). The ordering against a peek is the desktop's too and
it is not arbitrary - **a peek outranks a hide, and that is what makes a hide
cancellable**, since the row has to come back for you to reach the menu that
cancels it. It does not outrank `notify`, because a peek is a look at the chat
list and not a request to be interrupted.

**A bug worth recording, because the fix is the interesting part.** The hooks
went into `shown()` and `silenced()`, which is where the desktop puts them - and
nothing happened. `PurpleGate.filter()`, which is what actually builds the chat
list, does not call `shown()`: it inlines the same decision because it is also
counting what it saw, for the log line and the unread-gated tally. So the one
caller that mattered went straight past both hooks. The repair was not to add a
third copy of the test but to put the part that is not the preset's answer -
a peek, an "until" - behind one `byHand()` helper that all three call. A compile
proved nothing here; the emulator's count line proved it in one run.

**`hide_scope`, ported in both halves.** `hide_everywhere` is ported now, and so
is the preset-wide `hide_everywhere_p` it reuses. Both are answered at one seam,
`MessagesController.sortDialogs()` - the single method that rebuilds every list
derived from `dialogs_dict`, so a chat skipped there is missing from the chat
list, the folder tabs, the forward picker and the kind-limited pickers at once,
while `dialogs_dict` itself is left whole and the chat still opens from a
notification or a link. The share sheet and search build their own lists and ask
the same predicate for themselves. The default's launcher-badge
half is one test on the `countedForBadge()` seam A4 built, which already asks
exactly "does this chat's unread belong in the running totals". A community
row's folded badge and the folder list in the tabs activity's popup are
running totals over rows too, and ask the same question through
`countedInTotals()`.

Its **folder-tab** half is now done too, and the shape of the clients is what
made it a separate pass rather than a hard one. On the desktop a folder's unread
is a running total per list, which is why that side needed a cached flag and a
notifier to pay the difference across. Here it is recomputed from scratch by
`MessagesStorage.calcUnreadCounters()`, but not as a sum over dialogs: each chat
is tallied into a `contacts`/`nonContacts`/`groups`/`channels`/`bots`
`[folder][muted]` bucket, and every folder then adds up the buckets its flags
select. So taking one chat out is a guard on the bucket increment in each of the
three loops - users, encrypted chats, chats - in both of the passes that keep
these numbers: `calcUnreadCounters()`, which rebuilds them, and
`updateFiltersReadCounter()`, which moves them by a delta as a chat goes unread
or is read. The same guard goes on the per-folder exception walks that follow
each pass, over `alwaysShow` and `neverShow`, and there it matters in both
directions: a chat that was never added must not be added back through an
exception, and - the half that actually breaks things - a chat that was never
added must not be subtracted either, which is how a tab ends up showing a
negative number. Each pass logs one line when it dropped anything, which is the
only readable evidence that it ran.

The earlier note here said the change "reaches further than the scope's own
wording", on the grounds that the same buckets feed `mainUnreadCount`. That was
wrong, and worth correcting rather than quietly dropping: the scope says every
running total, and `keep_in_folder` is a promise about the **row**, not about the
sum. All chats is a running total like any other, so the main count and the
archive's were in scope from the start, along with every folder tab's and the
preset's own extra views.

What the guard keys on is the "hide until" and nothing else. A chat the preset
itself hides still reaches the totals, as a muted chat, through the hooked
`isDialogMuted` those loops already call - unchanged, and deliberate: a preset is
a standing arrangement about what you look at, an "until" is a decision you just
made about one chat, and only the second is worth rewriting a number over.

**The line** is `In 'Keep': shown`, or `In no list 'Work' names: hidden until a
mention`. Where comes from the entry that claimed the chat, asked of the
resolution rather than of the file - a list the preset does not name has no say
in what is happening now, however many members it has. What comes from the same
`shown()` the chat list asked, not from the mode alone, because a chat a folder
pulled back in is shown whatever its list said and a line reading "hidden" over
a row sitting in the list is worse than no line at all.

### The close buffer

Ported, and it completes the family: peek, the "until" decisions and this are
the three things that decide a chat without the preset having a say, and all
three now meet in one `byHand()` helper. `[recent]` is tested first of the
three, ahead even of a hide - it is not a statement about what the preset lets
through but about what you were doing ten seconds ago.

**The seam found itself.** The desktop hooks `setFakeUnreadWhileOpened()`
because that setter already means "this chat became, or stopped being, the one
you are looking at". Android has a method that means exactly that and nothing
else - `NotificationsController.setOpenedDialogId()`, called by `ChatActivity`
with the id on the way in and with zero on the way out - so the port is one line
at the top of it. Above the queue post rather than inside it, because the
eligibility test reads the chat list and the UI thread owns that.

Everything else follows the desktop. Eligibility is decided **when the chat is
opened**, not when it is closed, because two of the three scopes ask where the
chat was at that moment and reading it is precisely what moves the answer. The
clock starts on close, so reading something for five minutes does not burn the
grace before you have finished with it. One timer for all of them, armed for the
earliest deadline, because nothing else would ever bring the row back - no
message arrives and no unread moves when a clock runs out. And none of it is
persisted: a grace period that survived a restart would mean the app remembering
that you glanced at somebody yesterday.

Two small differences, both from Android's shape rather than from choice. The
grace map is a short list rather than a map keyed by id, because the account has
to be part of the key and only a handful of chats are ever in grace at once. And
`any_open_chat_except_in_folder` asks the *strip* - the folders the preset is
actually showing - rather than the account's folders, which is the desktop's
rule met through `shownFilters()`; the desktop also checks its extra views
there, and this is one of the places that will need them when they land.

`[recent] style`, which marks a row that is only there on a clock, is ported,
and it landed as one pass with the "show until" mark because both are the same
claim. One accessor answers for both: `PurpleGate.temporary()` mirrors
`History::purpleTemporary()`, returning the span on `elapsedRealtime()` and
whether a "show until" is what is holding the row - held or lingering, drawn the
same way in different colours. It carries the desktop's rules unchanged: a "show
until" and nothing else, a chat you have open is not counting down yet, and in
every branch the row must actually leave the view when the clock stops, asked
last because it is the expensive test.

`DialogCell` draws the two styles. The stripe goes down the row's leading edge
immediately after the row background and inside the swipe translation, so it
belongs to the row rather than to the strip behind it. The timer is a ring in
the badge slot, and only when nothing else wants that slot - a count, a mention,
a reaction mention or an error is a fact about the chat, where this is a fact
about how long the row has left. The pin is not in that reckoning here, because
Android draws it beside the date rather than in the badge slot. While a ring is
showing, the cell re-arms one repaint a second from the draw that needs it -
nothing else would come back for a mark that moves on a clock alone, and arming
it only from there is what stops it: a row that stops drawing a ring stops
re-arming.

The bridge carries two new fields for it: each override's start time as `from`,
so the ring has both ends of the span, and `recentStyle` alongside `recentSeconds`
and `recentScope`.

### Extra views

Ported, and cheaper than it looked, because A3 had already paid for it. An
extra view here is a **synthetic `DialogFilter`** - a folder object the account
never had - inserted into the *display* accessor `getDialogFilters()` and
nowhere else. Everything that edits, uploads, counts or limits folders reads
`getDialogFiltersUnrestricted()` and cannot see one. The two-accessor rule was
built for the folder strip; this is what it was really for.

**Membership rides the packed per-chat answer.** `visibleNative` returns one bit
per view alongside the show mode and the notify flag, so `PurpleGate.viewHolds()`
is a shift and a mask over the value the gate already caches. That matters more
here than anywhere else: the caller is `DialogFilter.includesDialog()`, asked
once per chat per sort for every tab that is showing, and a JNI call there would
be a call per chat per view. Sixteen is the core's own view limit, so the field
cannot overflow. One hook at the top of `includesDialog()` is the whole
integration - `sortDialogs()` then fills the tab exactly as it fills a folder's.

The rules a view follows are the core's, unchanged: it selects membership and
nothing else, the unread-watching modes are not honoured on it, and neither are
the per-kind defaults. That is what makes the useful pattern work, and it is the
one thing worth checking by hand after a change - a list can be **gated in the
main view and unconditional on a tab of its own**.

**Two things a view needs that a folder gets for free.** Its badge is walked
rather than read: `filter.unreadCount` is summed from buckets a synthetic filter
was never counted into, so it would read zero over a tab full of unread chats.
The walk runs only when a preset declared a view. And its pinned order is its
own - the file's `pinned` holds bare ids, so the sign has to be recovered before
the comparator can match a dialog id, and a pin naming a peer the client has not
loaded yet is skipped until the next reload. That is the same cold-start hole
the folder walks have and it is harmless for the same reason: there is no row to
order either.

A view restricts the strip whether or not a peek is running, and that test comes
first, because extra views sit on the strip and a peek leaves them exactly where
they were - so a strip index no longer matches a server-side position even
mid-peek. A peek does not fill a view, either: a view hides nothing, and filling
it with every chat for two minutes would only take it away.

### Editing the file on the phone

Everything above assumed the file arrived from somewhere else.
`settings.toml` lives in app-private storage, so on an unrooted phone nothing
but this app can open it - and the only thing the app itself ever wrote into it
was list membership, one line at a time. Everything else had to be written on a
desktop and carried over. Three screens close that, and none of them is a second
source of truth: each reads the file it is about to change, writes through the
splicer, and lets the reload decide what the app now believes.

**The Purple settings screen** is a `UniversalFragment` reached from Settings,
where the old entry opened the picker directly. Work Mode, with the running
preset as its subtitle; the schedule, with its next boundary; Local Premium and
"hide hidden chats from suggestions" as checks; and then the settings file
itself - its status, the editor, sending it to Saved Messages, checking Saved
Messages for a newer one, importing from a file, sharing it out, and the path,
which copies when tapped. The checks read the file rather than a preference and
are rebuilt from `PurpleGate.state()` whenever the list updates, so a write that
fails leaves the switch where the file is without a line of code to put it
there.

Two things are deliberately not on it. `PurpleDefaults` values are defaults
rather than settings - a thing this fork decided before you configured
anything, and [defaults.md](defaults.md) is where they are argued. And
`hide_archive_p` is per preset, so it belongs in the preset, not in a global
switch that would have to mean something under Normal.

**The editor** is a monospace text box with the action bar's tick as Save. It
validates in the background about a third of a second after you stop typing, by
calling `PurpleCore.parse()` on the bytes in the box - a pure function that
touches no file, which is the only reason live validation is affordable at all.
The status line says OK, or "N warnings", or the parser's own
`LINE:COLUMN: message`, and tapping a red one moves the cursor there. Save
refuses a file that does not parse, refuses one over the 64 KB ceiling, refuses
when the file changed on disk since it was opened, writes `settings.toml.bak`
first, and then shows the warnings - which the app had until now been throwing
away.

**The schedule screen** is the ruleset list. A row per ruleset -
`phone - Mobile - Enabled` - with the flat `[[schedule.rules]]` array shown as
`Rules` when the file has any, an `Add ruleset` row, and the status line above
them saying what is running now and on which device. A file that never grew a
ruleset shows the one `Rules` row and reads as the screen always did.

The top-level **Outside these windows** row shows `[schedule] outside` as the
file spells it, because that is the key it writes - not what this device works
out from it, which a ruleset naming its own `outside` overrides. When the two
differ the line under the row names the ruleset that is winning and what it
runs instead. Without it a save would look like it never took: the row would
read back exactly what was written while the schedule went on doing something
else. Android's schedule screen says the same thing in the same place.

Tapping a ruleset opens the rules it holds, with three rows above them. **Mode**
is Disabled, Enabled or Always. **Applies to** offers Any, Desktop, Mobile,
Android, iOS, This device, and `Other device...` with a name field - the last
listing whatever `[devices]` has named, so picking the laptop from the phone is
choosing from a list rather than retyping a hash. **Outside these windows** is
the ruleset's own `outside`, with "leave it to the schedule" as the first
choice, because leaving the key out is not the same as writing `normal`.

Under the rules themselves nothing changed: the window, the days and the preset
on each, a check for `enabled_p`, and an editor behind a tap with the preset by
radio list, seven day chips and both times through the system time picker. Rules
are addressed by their raw position and every write carries the window and the
preset the screen read off the rule, so a screen left open while the file
changed underneath refuses rather than rewriting the wrong one. The ruleset
goes with that address by **name**, never by position - a ruleset moves whenever
one above it is added or removed, and an index read a minute ago would edit the
wrong block. Rules the parser threw away are shown greyed with their warning
text - they are not in the rule list at all, so the screen recovers them from
the warnings, which name a rule by the same position, counting from one.

**The settings screen gained a "This device" row**, showing the id and the label
`[devices]` gives it, and writing that label when tapped. It is the only place
the phone's own id is legible, and a ruleset that names a device has to be typed
against something.

The writes themselves are three new splice ops in the shared core -
`SetScheduleRule`, `AppendScheduleRule`, `RemoveScheduleRule` - built to the
same contract as everything else that edits this file: replace the value where
it stands, re-parse what came out, compare it against what was intended, and
refuse rather than hand back a file to repair by hand. Comments, spacing and
keys the app has never heard of come through untouched. `[schedule] enabled_p`
found the one hole in `SetTableBool` on the way: a `[schedule]` with no header
of its own is an implicit table pointing at its first rule, so the insert went
into the rule. It now writes the header, which fixes it for every caller.

### Stories

Ported, and the desktop's one rule holds: filter the strip, and nothing else.

The seam is one accessor. `StoriesController.getDialogListStoriesShown()` is
the filtered view of `dialogListStories`, and everything that draws the strip
reads it - the strip itself, its collapsed three-avatar form, the "is there
anything to draw" test `DialogsActivity` gates the cell on, and the chain the
story viewer walks onward along as new stories arrive under it. The raw list
stays where it was, because it also feeds the counters, the archive strip and
the upstream hidden/unhidden machinery, none of which has anything to do with a
work preset. That is `dialogs_stories_content.cpp`'s reasoning met in Android's
shape: one place that is only about the strip.

`Purple::StoryShown()` becomes `storyShownNative`, and it takes two arguments
the desktop's does not: what the folders holding this chat said about its
stories, and whether one of them pulls the chat into the view. Both are folder
questions, and folder membership is Java's to answer here - the core has never
heard of a Telegram folder, which is the same reason `exemptFolders` and
`silencedFolders` arrive as names. Everything else is the desktop's function
line for line, including the order: a peek reveals, then a folder beats a list
entry, and both beat the preset's policy.

`hasUnseen` is passed in on this side too, and it buys the same thing: the
caller has the unread state right where the filter runs. What is different is
that the answer is cached. `PurpleGate.storyCache` keeps both halves of the
seen ladder per chat in one int and is cleared with `modeCache` on every
reload, so a chat costs one native call per resolution rather than one per
pass - and the second half is a second call at fill time rather than a call
every time, because a story's seen state flips exactly once. It is filled only
for a preset that mentions no folder, which is nearly all of them: folder
membership moves without a reload, and a cache with no way to hear about that
would go stale in exactly the case it was built for.

The strip is rebuilt on a preset change by posting `storiesUpdated` from
`postRefresh()`, next to the two signals already there. `DialogsActivity`
listens for it and re-runs `updateStoriesVisibility()`, which is both the
visibility test and the rebuild. Same reason as the desktop's merge of
`Purple::ActiveChanges()` into the strip's producer: nothing about the sources
changes when a preset does, so without it the strip would show what the last
preset let through until the next story arrived.

Two consequences worth stating. Your own row goes through the gate like anybody
else's - Saved Messages has no exemption from the lists, and `hasOnlySelfStories()`
had to learn about that or the cell would have stayed on screen with nothing in
it, since `DialogsActivity` draws it when *either* that or `hasStories()` is
true. And the count in the strip's title counts what the strip is showing while
a preset filters, rather than what the server said, which is `Content::total`
moving after the loop by another name: a title announcing five stories over two
avatars is the leak said out loud.

### Not ported yet

Nothing of A5 proper is left. Hot reload, the list-membership menu, peek, the
schedule, the "... until" overrides, the line naming which entry decides a
chat, the `[recent]` close buffer, extra views and, since 2026-09-06, the
launch-time offer of a settings import are all done; see above and
[sync.md](sync.md).

The two smaller pieces that used to wait here - the folder-tab half of
`hide_scope`'s default, and the chat-list mark for a row that is only present
on a clock - landed on 2026-09-06, each described in its own section above. The
`folders` key itself is complete: the tab, `notify_p`, `badge_p` and
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
        purple_screentime.{h,cpp} the screen-time log, not recorded yet
        purple_state.{h,cpp}      state.toml, the cache
        purple_splice.{h,cpp}     the surgical writes
    Telegram/SourceFiles/purple/purple_config.{h,cpp}     file IO and watcher
    Telegram/SourceFiles/purple/purple_focus.{h,cpp}      OS focus sync
    Telegram/SourceFiles/purple/purple_gate.{h,cpp}       the seam to tdesktop
    Telegram/SourceFiles/purple/purple_last_seen.{h,cpp}  reasons and the trade
    Telegram/SourceFiles/purple/purple_list_menu.{h,cpp}  list membership menu
    Telegram/SourceFiles/purple/purple_peek.{h,cpp}       the peek hotkey
    Telegram/SourceFiles/purple/purple_preset_box.{h,cpp} the preset picker
    Telegram/SourceFiles/purple/purple_schedule.{h,cpp}   the schedule clock

Every file in the submodule has no tdesktop dependency at all - which is what
lets the Android app compile them verbatim. The engine never sees a
`PeerData`, only an id and a `ChatKind`, for the same reason the parser does
not: every policy here is a rule about data, and rules about data are far easier
to prove outside a running app. `purple/test_config.sh` compiles them
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

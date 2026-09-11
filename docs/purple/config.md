# Fork configuration

Purple Telegram keeps its own settings in plain TOML, outside Telegram's
encrypted `tdata`:

    $XDG_CONFIG_HOME/purple-telegram/settings.toml
    $XDG_CONFIG_HOME/purple-telegram/state.toml

falling back to `~/.purple-telegram/` when `XDG_CONFIG_HOME` is unset.
`settings.toml` is created with commented defaults on first run.

Three more files appear in the same directory once screen time is switched on:
`screentime.log`, the append-only event log, `screentime_snoozes`, the day's
snooze count for a hard budget's cover, and `screentime_notices`, the soft
budget bulletins already shown today. None of them exists while
`[screen_time] enabled_p` is false, and none of them ever leaves the machine.

## Why a separate file

Telegram's own settings are a single serialized blob inside `tdata`. Adding a
field to it means editing `Core::Settings::serialize()` and
`addFromSerialized()`, which is the one place in the codebase where a mistake
corrupts saved settings, and which upstream appends to regularly - a guaranteed
rebase conflict.

A separate file avoids all of that, survives a Telegram data-directory reset, and
can be fixed by hand if the app ever refuses to start. The tradeoff is that it
sits in plaintext rather than in the encrypted store. That is fine for feature
flags and deliberately not fine for anything secret; API credentials stay outside
the repository in `../tdesktop-libs/api_credentials.sh`, as
[docs/mac/build.md](../mac/build.md) describes.

## Two files, two owners

`settings.toml` is yours. The app reads it and makes only surgical writes to it:

- the Premium toggle in Settings, which rewrites the single value token of
  `[premium] enabled_p`,
- adding and removing list members, which edits one line of a `members` array.
  That is the `Work Mode` submenu on a chat's context menu - see
  [work_mode.md](work_mode.md).
- creating a list, from `New list...` at the foot of that same submenu, which
  appends an empty `[lists.x]` table after the last one already there,
- pinned orders, when a preset or one of its views owns one - dragging a pinned
  row rewrites that array.

The one write that is not surgical is an import from Saved Messages, which
replaces the whole file after asking - see below.

Neither re-serializes the document. Both locate what they need through toml++
source regions and edit the raw lines, so comments, blank lines, alignment and
key order all survive. `enabled   =    true   # keep ads away` comes back as
`enabled   =    false   # keep ads away`, spacing and comment intact.

`state.toml` is the app's. It holds the active preset and why it is active, the
focus-sync memory, the schedule's pause flag, the moment that pause runs out and
its last target, the peek timer, and the last seens read by trading. It is
rewritten
whenever any of that changes, carries no comments, and preserves nothing. That
is the entire reason it is a separate file: state churns constantly, and it must
never touch the mtime of the file you are editing by hand.

It is reloaded live too, which is not something a machine-owned file needs but
keeps it hand-editable now that the preset box is a second writer to the same
fields - see [work_mode.md](work_mode.md). The app's own writes come back
through the same watch, compare equal to what it just wrote, and do nothing.

Both are written through `QSaveFile`, which writes a temporary alongside the
target and renames over it, so a crash mid-write cannot leave a truncated config.

## The third file, which is nobody's

`readme.md` in the same directory is generated. It is written from `kReadme` in
`purple_readme.cpp` on every launch, only when what is on disk differs, and
anything hand-written into it is lost the next time the app starts - which it
says in its own first line.

It exists because the config directory taught nobody anything. `settings.toml`
carries comments, but the schema lived only here, in a repository the person
editing the file may not have checked out. A reference beside the file being
edited answers the question where it is asked.

It is a different document from this one on purpose. This file is about why the
config works the way it does; `readme.md` is about what you may type, in the
order the questions arrive: the keys, then the defaults, then what to do when it
is not doing what you meant. Design reasoning stays in
[work_mode.md](work_mode.md). Both have to be updated when a key changes, and
the parser is the arbiter of which one is right.

The only thing in it that is not a constant is the path to `log.txt`, which is
substituted from `cWorkingDir()` because it is the one line nobody can guess.

## Syncing between devices

`settings.toml` is per-install: two machines have two files, and nothing keeps
them in step by itself. Two actions move one to the other, with Saved Messages
as the transport:

- **Settings > Advanced > Purple > Send settings to Saved Messages** posts the
  current file to your own Saved Messages as a document called `settings.toml`,
  captioned with its schema version, the local time and the platform. It asks
  first, because the file names chats.
- **Right-click that message > Import Purple settings**, in Saved Messages,
  reads it back. It parses before it writes, so a file that is not valid TOML
  is reported and nothing is touched; otherwise it shows the date, the schema
  version and the number of parser warnings, and replaces the file on confirm.
  The watcher then reloads it like any other edit.

Saved Messages is the version history. Every send is a message with a date on
it, they stack up in one chat, and picking an older one is picking an older
config - so there is nothing here that keeps revisions, because the chat
already does.

The previous file is kept as `settings.toml.bak` beside it. There is exactly
one, overwritten on every import: it is there for the "that was the wrong
file" moment, and a numbered series would silt up a directory people read by
hand when Saved Messages is already holding every version worth going back to.

`state.toml` is not sent and not touched. The active preset and the running
timers are what *this* machine is doing, not what your settings are.

See [sync.md](sync.md) for why it is Saved Messages and not git or a server.

## Editing it while the app runs

`settings.toml` is watched and reloaded live. The watch is on the directory
rather than on the file, because editors that save by writing a temporary and
renaming it over the original leave a file watch pointing at an inode nobody
will ever write to again. Reloads are debounced by 250ms, since a single save
can produce several filesystem events and reloading halfway through one would
flash an error banner every time you hit save.

## What happens when it is wrong

The file is hand-written, so nearly everything recoverable is a warning that
leaves usable settings behind rather than a hard failure:

- An entry naming a list with no `[lists.x]` table claims nothing, and says so.
  It looks identical to an entry that is working, which is exactly why it warns.
- A `"*name"` reference to a set that does not exist is dropped. So is one that
  refers back into itself, however many hops it takes.
- A name mentioned twice in the same directive keeps its first mention. That is
  forced for a `list_order`, where order *is* capture, and folders follow the
  same rule so there is one to remember rather than two. The duplicate is
  reported unless the earlier mention came from a spread - overriding one entry
  and then splicing in the defaults is the idiom spreads exist for.
- A preset named `normal` is refused; that name is the bypass.
- A preset that names no list at all is legal and hides everything, which is
  said out loud rather than left to be discovered.
- A schedule rule or focus-sync action naming a preset that does not exist is
  skipped. A *disabled* rule is left alone, so the example in the starter file
  does not complain on every start.
- Keys the old model used - `show`, `notify` and `locked` on a list, `inherit`
  and `overrides` on a preset, `show_p` and `groups_require_mention_p` on an
  entry, `filtered`, `include_in_main_view_p` and `pinned_only_p` on a folder,
  `enabled` anywhere, a top-level `list_order` -
  are reported by name, with what to write instead. Silence there would let a
  file that is doing nothing look like one that works, which is exactly what a
  folder still saying `include_in_main_view_p` would do: include nothing.
- Duplicate member ids are deduplicated on read, keeping the order you wrote.
- A list whose name starts with `*` is refused: nobody reading the file could
  tell it from a set reference.
- A `version` newer than the build understands is warned about and then read
  anyway; one that is not a whole number, or is below 1, is warned about and
  read as 1. A missing one is not warned about at all.
- A hotkey that a message field has already claimed for a formatting action is
  warned about, naming the action. Such a key works everywhere except with the
  composer focused, where Qt calls the sequence ambiguous and fires neither
  binding - silently. The reserved ones are `Ctrl+B`, `Ctrl+I`, `Ctrl+U`,
  `Ctrl+K`, and `Ctrl+Shift+` with `X`, `M`, `N`, `P`, `D` or `.`.

Only one thing is fatal, because it leaves nothing meaningful to run on: a TOML
syntax error. The app keeps the last settings that worked, logs the line and
column, and refuses to write to the file at all - you may be halfway through an
edit, and a blind write would leave you with a mess to untangle on top of
whatever you were already fixing.

Everything the app could not make sense of is available to the UI as a warning
list, for the banner described in the Work Mode spec.

## Schema

`version` at the top of the file is the schema the file is written to. It is
there so a build that meets a file from a newer one can say so, rather than
quietly doing half of what the file asks:

- **Absent** is version 1, silently. Every file written before the key existed
  is a version 1 file, and there is nothing to report about that - a config
  that started warning the day it was read by a newer build would teach you to
  ignore the banner.
- **Higher than the build understands** warns, naming both numbers, and the
  file is read anyway. The keys this build knows are still where they were, and
  the ones it has never heard of were already being ignored.
- **Not a whole number, or below 1**, warns and is read as version 1.

The number moves only when a key changes *meaning*, since that is the only kind
of change an older build gets wrong rather than merely misses. Adding a key
does not move it. Bumping it every release would make it noise.

It says nothing about your messages. How recent they are, and what has and has
not been synced from the server, live in `tdata` and are none of this file's
business; this number describes the shape of `settings.toml` alone.

`[premium] enabled_p` controls the client-side Premium unlocks; see
[premium.md](premium.md) for what that covers and what it deliberately does not.

Every boolean key in this file ends in `_p`. That is a naming convention, not a
type distinction - it just means the answer is yes or no, and it makes a flag
recognisable as one without looking it up.

The Work Mode half is two ideas. A **list** says who is in it: `members` by peer
id, `kinds` by chat type (`private`, `groups`, `channels`, `bots`), or both. A
**preset** names the lists it wants in a `list_order`, each entry carrying
`show_mode` and `notify_p`. Order is priority and capture: the first entry whose
list holds a chat decides it, and nothing further down sees that chat. A chat no
entry claims is hidden and silenced.

`show_mode` is `"always"`, `"message"`, `"message_or_reaction"`, `"mention"` or
`"never"`, and the three in the middle depend on the chat's unread state, so a
chat comes and goes on its own. Leaving it out takes the default for what the
chat *is*: a channel or a bot is `"always"`, a group is `"mention"`, a private
chat is `"message"`. It replaces `show_p` and `groups_require_mention_p`, both of
which are now retired keys that warn with the replacement.

`folders` names the account's real folders, each with `enabled_p` (false makes
the preset ignore the entry outright - see below), `show_p` (its **tab** is in
the strip - the only one of the rest about the tab rather than the chats),
`notify_p` (false silences its chats), `badge_p` (false takes the folder out of
every count - no number on its tab, its chats out of the app badge), `show_mode`
(the mode its chats take, on the same terms as `notify_p`; leaving it out gives
each chat the default for what it is) and `include_in_main_view`,
which is `"none"` (the default), `"pinned"` or `"all"` - how much of the folder
joins the preset's own view whatever the lists decided. What it lets in comes in
even if the chat is archived, since under a preset the preset is what controls
visibility. A preset with no `folders` key shows no folder tabs; `"*ALL"` is
every folder you have.

`enabled_p = false` is the comment character you do not have to add and remove.
The entry keeps everything configured on it and does none of it: no tab, nothing
silenced, nothing fed into the view, nothing counted. It is not the same as
`show_p = false`, which only takes the tab off the strip while the folder goes
on silencing and feeding and counting.

It also beats `"*ALL"`. A folder switched off by hand stays off in a preset that
also asks for every folder - otherwise switching one off would quietly stop
working the day you added the spread.

`hide_archive_p` on a preset puts the archive away with everything else that
preset hides: no row for it in the chat list, and on Android no pull gesture at
the top of the list either - the state an account that has never archived
anything is in.

Leaving the key out means **yes**, which is the opposite default from
`hide_everywhere_p` and is meant to be. The archive is where you put what you
are not dealing with, so a preset that has already named what gets through has
no reason to leave a door open to the rest of it. Write `hide_archive_p = false`
to keep the archive where it was.

It is only ever asked while a preset is filtering, so Normal is untouched, and
it says nothing about what is *inside* the archive: an archived chat a list
names, or a folder pulls in, is in the view either way. On the desktop there is
no pull gesture and the Archive row already lives in the main menu rather than
at the top of the list, so nothing here consumes the key yet - it is parsed,
carried and resolved so that one file means one thing on both, and it is the
Android app that acts on it.

### Marking a row that is only there for the moment

`after_close_chat_style` in `[recent]` says how the chat list marks a row that
is in the view on a clock - one inside its close buffer, or one a **Show until**
is holding open. Both leave by themselves, and neither is otherwise
distinguishable from a chat the preset simply allows.

**Only a row that will actually go is marked.** The close buffer holds every
chat you open, including the ones the preset shows anyway - and those are not on
a clock, so they carry nothing. What is marked is the row the preset would hide
the moment the clock stopped: a chat gated on unread that you have just read, or
one nothing in the preset claims at all.

- `"none"` - the default. Nothing is drawn.
- `"stripe"` - a bar down the row's left edge.
- `"timer"` - a ring where the unread badge would sit, emptying as the time
  runs out.

The two reasons are told apart by colour. The close buffer takes the accent the
unread badge already uses; a **Show until** is green. They are not the same
claim - one is the app keeping a chat you just closed, over in a couple of
minutes, and the other is a decision you made and will want to recognise hours
later as the reason a chat is sitting somewhere it does not belong. On the row
you have selected both go white, like everything else on it.

```toml
[recent]
stay_visible_after_close = "2m"
after_close_chat_style   = "timer"
```

`timer` only takes the badge slot when nothing else wants it: a count or a pin
is a fact about the chat, and those win. It is also the only style that costs
anything - a row has to be redrawn once a second for a ring to move, so a
one-second tick runs while any row carries one and stops as soon as none does.

Neither style is cached. The chat list caches rendered rows by entry alone, so
a baked-in stripe or a frozen ring would outlive the thing it was reporting;
rows carrying a mark are drawn fresh instead. There are only ever a handful.

### Show until, Hide until, Notify until

Three entries in the `Work Mode` submenu, each offering 30 minutes, 2 hours, 8
hours or 24 hours. They are decisions about one chat that outrank the preset
until they run out:

- **Show until** puts the chat in the view past the preset's rules. It does not
  un-silence it - visible and quiet are different questions.
- **Hide until** takes it out of the view, and silences it while it is gone,
  because a notification from a chat you have deliberately hidden is
  incoherent.
- **Notify until** lets a preset-silenced chat interrupt you. It lifts **only**
  the preset's mute; a chat you muted yourself stays muted, which is the rule
  the whole mute path is built on.

A fourth entry, `Cancel '... until'`, appears only while one is running.

They are **scoped to the preset they were made under**. "Show this until six" is
a statement about the work you are doing now, and carrying it into every other
preset would be a larger promise than the menu offered. Switching away and back
keeps a live one; it simply does nothing while another preset is running.

They live in `state.toml`, not here - they are the app's, they expire by
themselves, and they must never touch the mtime of the file you are editing.
The deadline is a unix timestamp rather than a monotonic clock, because unlike
the `[recent]` buffer these are measured in hours and have to survive a
restart.

#### How far a Hide until reaches

A hide always takes the chat out of the preset's own view. What it does about
the folder tabs - where the chat is a member of a list the preset does not own -
is up to you:

```toml
[overrides]
hide_scope = "keep_in_folder_but_exclude_from_badge_count"
```

- `keep_in_folder_but_exclude_from_badge_count` - the default. The row stays on
  its folder tabs, but its unread stops counting towards them, so the folder
  does not sit there with a badge for a chat you have just put away. The row
  keeps its own count: what changed is what the tab adds up, not what the chat
  says about itself.
- `hide_everywhere` - out of the chat list altogether, the way
  `hide_everywhere_p` does it for a whole preset. No row on any tab, and no
  count anywhere, until it runs out.
- `keep_in_folder` - the row stays and keeps counting. This is what a hide did
  before the key existed, and it is still a coherent thing to want: out of my
  view, but still part of that folder's total.

The key is global rather than per-preset because the menu offers the same
decision whichever preset is running.

A **peek suspends all of it**, exactly as it suspends the hiding itself: while
you are peeking the row is back, and so is its contribution to every count. So
is a chat sitting in the `[recent]` close buffer, which is a chat you have open.

### Making a list from the chat menu

`New list...` at the foot of the `Work Mode` submenu appends an empty table:

```toml
[lists.reading]
title = "Reading"
members = [
]
```

and nothing else. **The list does nothing until a preset names it.** There is no
write that edits a `list_order` - those hold strings and inline tables, and
nothing in the app writes those - and guessing which preset wanted the new list
would be worse than saying so, which the toast does.

Tick chats into it from the same menu straight away; add
`{ list = "reading" }` to a preset's `list_order` by hand when you want it to
start deciding anything.

One limitation, kept on purpose: the whole `Work Mode` submenu is left out until
`settings.toml` defines at least one list, so an unconfigured fork's menus are
untouched. That means the *first* list has to be written by hand. Every one
after it can come from the menu.

### Pinned chats in the main view

`pinned` on a preset is a list of peer ids, and writing it changes who owns the
main view's order.

Without it - the default - the preset's main tab **mirrors** your account's
pinned chats, minus whatever the preset hides. Pinning there pins in the chat
list, travels to the server, and is capped at the account's five.

With it, the preset **owns** the order. A pin made inside the preset stays
inside it: it is written back to `settings.toml`, never reaches the server, and
so leaves your account's order and your other devices exactly as they were. That
is also what lifts the five-chat ceiling, since five is the server's limit on
the account order the mirror was mirroring, and a list kept in this file is
bounded by nothing.

```toml
[presets.work]
pinned = [ 1234567890, 987654321 ]
```

Dragging a pinned row inside the preset rewrites this array, the same way
dragging one in an extra view rewrites that view's `pinned`. Delete the key to
go back to mirroring.

### Stories

`stories` on a preset says what the stories strip does while it runs. Five
values, each hiding strictly more than the one before it:

- `"all"` - no filtering. The strip is exactly stock Telegram.
- `"all_unseen"` - everybody, but only while they have something you have not
  seen. A person drops off the strip once you have watched everything they
  posted.
- `"follow"` - the default. Hides whoever the preset excludes **outright** and
  keeps whoever it merely has nothing to show you from.
- `"follow_unseen"` - `follow`, and only while unseen.
- `"none"` - no strip at all.

The distinction inside `follow` is the whole idea. A person the preset excludes
outright - nobody's list claims them, or one claims them with
`show_mode = "never"` - loses their story, because the preset already refused
them. A person the preset admits and is only holding back for being quiet -
`"message"`, `"message_or_reaction"`, `"mention"` - keeps theirs, because a
story *is* new activity, which is exactly what those modes are asking to be
shown. Suppressing the one thing such a person does have would invert the
setting.

A `list_order` entry or a folder entry can override that for **its own** people,
with a narrower vocabulary:

- `"always"` - their stories show, seen or not, whatever the policy says.
- `"unseen"` - only while unseen.
- `"never"` - none of theirs.

There is no `"all"` here, and writing one warns: an entry is already a set of
people, so "everybody" would mean nothing. The three spellings are the ones
`show_mode` uses next door, which is the point - they sit in the same inline
table.

```toml
[presets.work]
stories = "follow_unseen"
list_order = [
  { list = "close_people", show_mode = "message_or_reaction", stories = "always" },
]
folders = [ { name = "Music", stories = "never" } ]
```

A folder beats a `list_order` entry, the same way a folder already beats one for
hiding, and both beat the preset's policy. A peek reveals stories along with the
chats they belong to.

Note that your own "add a story" button is governed like anybody else's, since
Saved Messages has no exemption either (below) - under `follow` it goes away
unless a list names you. Write `stories = "all"`, or put your own id in a list,
if you would rather keep it.

### Saved Messages

Saved Messages is an ordinary chat to the lists. It has no exemption: a preset
that does not name it hides and silences it like anything else, because a preset
names what gets through and a chat that ignored that would be one you could not
reason about from the file.

To keep it, put it in a list. The easy way is the chat itself - right-click
Saved Messages, Work Mode, and pick the list, exactly as for any other chat. The
app writes your own user id into that list's `members`:

```toml
[lists.os]
title = "OS"
members = [
  1234567890, # Saved Messages
]
```

There is nothing special about the id; it is your account's user id, and the
trailing comment is regenerated like every other. If you would rather write it
by hand, Settings > My Profile shows it, or add it from the menu once and read
it back out of the file.

A bare `"*name"` string inside either array splices in `[list_sets.name]` or
`[folder_sets.name]`, the way Python spreads a list. That is what replaces
preset inheritance: reuse without a chain to walk when you are trying to read
what a preset actually does.

`[[presets.x.views]]` adds a tab of its own, with a `name`, an optional `pinned`
order and a `list_order` that selects membership only. The tabs are drawn after
the preset's main view and before any folder, each with its own unread badge and
its own pinned order. `pinned` is the tab's, not the account's: pinning a chat
there does not pin it in the chat list, and the app writes the array back the
same way it writes list membership. A view with no `pinned` sorts by date.

`[recent]` keeps a chat in the view for a while after you stop looking at it,
which is the one thing an unread-watching `show_mode` cannot express on its own:
reading a chat is exactly what takes it out. `stay_visible_after_close` is a
duration in the same spellings as `auto_off`, off unless written, and the clock
starts on close rather than on open. `applies_to` is `"already_in_view"` (the
default), `"any_open_chat"` or `"any_open_chat_except_in_folder"` - the last
skipping chats that are already reachable on an extra view or in a shown folder.
It is per-install rather than per-preset, and lives in memory only.

`[suggestions] hide_invisible_p` decides whether the strips of chats the app
offers you unasked leave out what the running preset hides. True unless you say
otherwise, and asked only while a preset is filtering.

It covers the people and recent rows in the search panel, the quick-share popup
a message's share button opens, the frequent contacts offered when you choose
who to send a gift to, and on Android the recent searches, the share sheet's
hints and the direct-share targets the OS itself keeps - including the
conversation shortcuts a launcher icon's long-press offers.

It never touches typed search, the forward picker, the share box or the
Ctrl+Tab switcher. Those are the places you went looking for a chat by name,
and keeping a chat out of a suggestion is a different claim from making it
unreachable: the second one is `hide_everywhere_p`, it is per preset, and it is
yours to ask for separately.

`[suggestions] recommended_channels_p` is the other key in that table, and it
is the only one there that defaults to **false**. It decides whether the
"similar channels" strip - the recommendations under the Channels tab of global
search, and the ones a channel offers when you open it - is shown at all.

It is off by default because it is the one suggestion that is not assembled out
of your own chats: the server picks it. Everything else `[suggestions]` covers
is a strip of people you already talk to, filtered; this is a strip of channels
you do not, proposed. A fork about deciding who reaches you should not be
switching that on for you.

Unlike `hide_invisible_p` it is asked under every preset, Normal included,
because it is a claim about the strip itself rather than about what a preset
lets through. Writing `recommended_channels_p = true` brings the strip back
everywhere, which is stock Telegram's behaviour.

The "My channels" list beside it is a strip like any other and follows
`hide_invisible_p`, not this key: those are your channels, and hiding one is
the same decision as hiding any other chat.

Like `[peek]`, `[recent]` and `[overrides]` the table sits outside the presets,
because it is a decision about those strips rather than about what any one
preset lets through. The test `hide_invisible_p` applies is the preset's alone
- no close buffer and no "until", so a chat does not drift in and out of a
strip while a timer runs - and a chat the preset still keeps one click away, on
an extra view or in a folder whose tab is showing, was never hidden and stays.

The rest - `[focus_sync]` and `[peek]` - is documented by the starter file the
app writes on first run, which carries a commented example of each.

### How long a peek lasts

    [peek]
    hotkey        = "Ctrl+Shift+E"
    auto_off      = "2m"
    tap           = "5m"
    hotkey_length = "2m"

`auto_off` is the length a peek has when nothing more specific is said, and
`"off"` is a peek with no clock on it. `tap` is what tapping the control
starts - the checkbox and the chips in the Work Mode box, the Peek row in the
Android picker - and `hotkey_length` what the desktop key starts; each falls
back to `auto_off` when it is not written, so one key still governs both for
anybody who does not care about the difference. `"off"` is a real answer for
either. The length key is not called `hotkey` because that name is the key
sequence, and it was documented and in use before there was a second way to
start a peek. A spelling the parser cannot read warns and leaves the key
unset, which falls back to `auto_off` rather than to a number nobody wrote.

Android reads `tap` and ignores `hotkey` and `hotkey_length`: a phone has no
key to bind. It has no starter file either, so a phone with no `tap` line taps
for `auto_off`; write `tap = "5m"` to get the longer look the plan asked for.

The lengths a control offers - 1, 2, 5, 10, 15, 30 and 60 minutes, then "until
I stop" one position past the last - are not configurable. They are the core's
row, so a phone's chips and a desktop's dial cannot come to offer different
minutes, and an extension by tapping or pressing again is capped at an hour
from the press: past that it is not a peek any more, it is the preset off, and
there is a plainer way to say that. See [work_mode.md](work_mode.md), "Peek".

### Why a last seen is coarse

    [last_seen]
    reasons_p      = true
    trade_p        = true
    trade_hold     = "10s"
    trade_remember = "24h"
    trade_cooldown = "5m"

`reasons_p` decides whether a status line says *why* it will not give an exact
"last seen". There are only two answers it will ever give - your own privacy
caused it, or nothing is added - and the reasoning behind that, including why
"a long time ago" is never explained, is in [work_mode.md](work_mode.md). It
governs every place a last seen is written, not only the chat header and the
profile: member lists, the pickers, the chat preview popup and the short info
box get the eyes mark where those two get the words.

What it does *not* govern is a read a trade came back with. That line is shown
with `reasons_p` off, because it is not the fork explaining a status - it is
the fork showing the answer to a question asked out loud, and a switch that
turns explanations off should not swallow it.

`trade_p` decides whether a line also offers the one-off trade: show them your
last seen for a moment, read theirs, put your privacy back. Turning it off
leaves the explanation and takes away the offer, and it is what the sheet's
"Don't offer this again" writes. It is the only key that decides whether a line
is tappable - both the reason tail and a remembered read open the sheet while
it is on, and neither does while it is off - and it takes the profile's own
button with it.

`trade_hold` is how long to wait for their status after asking before restoring
your rules. Ten seconds by design: the whole exposure is that window, and a
trade that has not answered in ten seconds is not going to.

`trade_remember` is how long a read stays worth showing. Past it the record is
dropped rather than shown as older and older news - "as of 3 min ago" is useful
and "as of 2 days ago" is not.

`trade_cooldown` is the least time between two trades with the same person.
Inside it the sheet still opens, counts the wait down and offers the re-trade
once it is spent, rather than refusing in a toast.

All three durations are written the way every other duration in this file is -
`"10s"`, `"5m"`, `"24h"` - and a spelling the parser cannot read warns and keeps
the default rather than guessing at a number. Like `[peek]`, `[recent]`,
`[overrides]` and `[suggestions]`, the table sits outside the presets: it is a
decision about how a status line reads, not about what any one preset lets
through.

The reads themselves are in `state.toml`, and they never leave the machine:

    [[last_seen_trades]]
    peer = 123456789
    read_at = 1757320000
    was_online = 1757319120

`peer` is the plain numeric id this file uses everywhere. `read_at` is when the
trade ran, in unix seconds, and is what both the age in the status line and the
cooldown are measured from. `was_online` is the moment that was read, or `0` for
a trade whose hold ran out with nothing arriving - still written down, because
the cooldown counts attempts rather than successes.

One record per person: a second trade with somebody replaces the first rather
than piling up, so the file is bounded by how many people you have traded with
and not by how often. Records older than `trade_remember` are dropped on the
next write.

### Screen time

    [screen_time]
    enabled_p      = false
    action_span    = "3s"
    active_gap     = "30s"
    idle_after     = "60s"
    retention_days = 90

    [[screen_time.budgets]]
    target         = "kind:channels"
    per_day        = "45m"
    mode           = "soft"
    snooze         = "5m"
    snoozes_per_day = 2

The desktop records and draws this; Android does neither yet. Switched on, the
desktop appends one line per event to `screentime.log` in this directory,
beside `settings.toml`, and everything Settings -> Advanced -> Purple -> Screen
time shows is derived from that file when you look at it. The log never leaves
the machine. See docs/purple/work_mode.md for the events, the active rule and
the cover.

`enabled_p` is false and stays false until you say otherwise. It is a log of
what you looked at and for how long, and nothing should start keeping one of
those because a version number moved.

The three durations are the thresholds a raw log is read back *through* rather
than recorded with: `action_span` is how long one send action counts as active
for, `active_gap` is how close two actions have to be for the whole gap between
them to count as active too, and `idle_after` is how long without any input
pauses a session. Applying them at read time is the whole reason the log stores
events instead of totals - changing one re-derives the history you already
have. `retention_days` is how much of the log to keep, with `0` keeping
everything.

`[[screen_time.budgets]]` is a day's allowance for one thing. `target` is a
single string - `"all"`, `"chat:<id>"`, `"kind:<kind>"` or `"preset:<name>"` -
because a budget counts exactly one thing, and four mutually exclusive keys
would let a file ask it to count two. The kinds are the chat kinds a list uses,
spelled the same, plus `"elsewhere"` for the time that was not in a chat at all
- the list, search, settings - so that the splits add up to foreground time
rather than to something smaller with no name.

`per_day` is the allowance, and `mode` is `"soft"` (a bulletin at the limit,
the default) or `"hard"` (the chat goes behind a cover naming the budget). A
hard cap never touches messages or notifications: it is a screen, not a mute.
`snooze` is how long one snooze lasts and `snoozes_per_day` how many are
offered; zero for either disables snoozing, so `snoozes_per_day = 0` makes a
hard cap absolute. The desktop keeps the day's snooze count in
`screentime_snoozes` in this directory, and the soft bulletins it has already
shown today in `screentime_notices` beside it, rather than in `state.toml`,
which is the core's schema and shared with Android.

The screen time box writes these. "Add a budget" under the list opens an editor
for the target, the allowance, the mode and - for a hard cap - the snooze; a
budget's own row opens the same editor on it, with Delete beside Save. A write
from the app is an edit to this file and reloads live like every other one, so
the two ways of writing a budget are the same way, and hand-editing still works
exactly as it did.

The app leaves `mode`, `snooze` and `snoozes_per_day` out when they are what
they would have meant anyway, and takes the line out again when a value goes
back to its default: a file where every budget spells out the defaults is
harder to read for no gain. An edit is addressed by the budget's position in
the array, counting the ones the parser threw away, and is refused unless the
budget still at that position says the target the box was opened on. So a box
left open while the file changed underneath cannot rewrite a budget nobody
looked at, and a broken budget in the middle of the file cannot shift the ones
after it. A budget the parser threw away is not on the list and so cannot be
edited from the app - fix its target in the file and it comes back.

A budget missing `target` or `per_day` is skipped with a warning naming its
position, and so is one whose target does not parse. The rest of the file is
read normally.

### Sending the file on every save

    [sync]
    send_after_save_p = false

`[sync] send_after_save_p` says whether a write the app itself makes to
`settings.toml` also posts the file to Saved Messages, where your other machines
find it. Off unless you ask for it, and it is meant to stay that way: sending is
a message in a real chat, so it should be something you turned on in words
rather than something an upgrade started doing on your behalf.

Only the app's own writes. Editing the file in a text editor does not post it -
that write arrives through the file watcher, and the fork does not treat a file
it did not write as a thing it was asked to send.

A run of writes is one document. The send waits five seconds after the last one,
so six checkbox taps in a row post what you settled on rather than six copies of
what you passed through.

Two keys in `state.toml` keep two machines from handing the same file back and
forth: `last_sent_fingerprint` and `last_imported_fingerprint`, each the length
and SHA-256 of a `settings.toml` written as `"<length>:<hex>"`. Nothing is sent
whose bytes match either one, and **an import never sends at all** - the bytes
came from the other machine, and returning them is exactly the loop this is here
to stop. Both are empty in an older `state.toml`, which is the same thing they
say on a fresh install, so the first send after an upgrade happens normally.

The whole of that rule is one pure function in the core, `ShouldAutoSend()`,
because the app around it - a network, a Saved Messages history, a file watcher
- is precisely the place a rule like this is hardest to prove anything about.
See [sync.md](sync.md).

### The schedule

Among the enabled rules covering a moment the **narrowest window wins**, not the
first one written. `08:00-17:00` for work with `12:00-14:00` for lunch cut out
of it gives lunch at one o'clock in either order, and two o'clock puts work
back. A window crossing midnight is measured the long way round, and two windows
of the same length keep the order they were merged in - ruleset specificity
first, then file position. Why it is nesting rather than position is in
[work_mode.md](work_mode.md).

`[schedule] outside` names the preset in force whenever no rule covers the
moment. It defaults to `"normal"`, which is what the schedule did before the key
existed, and it is a key at all because not every day has stock Telegram at its
edges - someone whose default is Home wants five o'clock to put Home back, not
Normal. A name no preset backs warns and falls back to `normal`, the same rule a
rule's own `preset` follows.

`schedule_paused_until` in `state.toml` is when a pause runs out, in unix
seconds, and `0` - which is what every older `state.toml` says, since the key
did not exist - means the pause lasts until it is lifted by hand. Unix seconds
rather than a countdown, for the same reason as the peek deadline: a pause is
measured in hours or days, so it has to mean the same thing after the app has
been closed and reopened.

### One file, many devices

`[[schedule.rulesets]]` blocks let one `settings.toml` describe several
machines. A ruleset names the devices it is for and whether it is off, on, or on
regardless; the enabled rulesets that apply to a device are merged into the one
schedule that device runs:

    [schedule]
    enabled_p = true
    outside   = "normal"

    [[schedule.rulesets]]
    name    = "phone"
    device  = "mobile"
    mode    = "enabled"
    outside = "home"

    [[schedule.rulesets.rules]]
    days   = ["mon", "tue", "wed", "thu", "fri"]
    from   = "09:00"
    to     = "17:00"
    preset = "work"

`name` is required, unique ignoring case, and the address every later edit goes
through. A ruleset with no name, or with one already taken, is skipped with a
warning.

`device` is `"any"` (the default), a class - `"desktop"` or `"mobile"` - a
platform - `"android"`, `"ios"`, `"macos"`, `"windows"`, `"linux"` - or a device
id outright. Anything else is read as a device id: the list of platforms is
closed, and the list of devices belongs to whoever is carrying them.

`mode` is `"enabled"` (the default), `"disabled"` or `"always"`. A disabled
ruleset is written down and switched off - a ruleset you are not using this
month is not a mistake in the file, so it is parsed and never warned about, just
never chosen. An `always` one is merged in on every device it applies to,
whatever else won, for the rules that are true everywhere.

`outside` on a ruleset overrides `[schedule] outside` for the devices that
ruleset runs on. Leaving it out is not the same as writing `"normal"`: it leaves
the question to `[schedule]`.

The flat `[[schedule.rules]]` array is not going anywhere. It is read as an
implicit ruleset called `rules` - device `any`, mode `enabled`, no `outside` of
its own - placed first, so a file written before rulesets existed resolves
through exactly the same path and means exactly what it always meant.

Which rulesets a device ends up running, and why the rule is specificity rather
than layering, is in [work_mode.md](work_mode.md).

`[devices]` gives ids names:

    [devices]
    "macos-3f9a2c1d" = "the laptop"
    "android-7b1e04c2" = "the phone"

Nothing depends on it - an id nobody named shows as itself - and it is in
`settings.toml` rather than on each machine so that the names travel with the
file that uses them.

### Addressing a rule, and a ruleset

A schedule rule has no name, and none is coming: it is addressed by its **raw
position**, the rule blocks counted from the top of the array it is in. A name
would be a second thing to keep in step with the file, and a screen editing a
rule already has to say which rule it read. That address is what an editor sends
back, together with the window and preset it read off the rule, so an edit lands
on the rule you were looking at or is refused - never on whatever is third now.

A rule the parser threw away keeps its position in that count. A broken rule in
the middle of the file does not shift the ones below it, and repairing it does
not shift them back, which is the only reason editing a file with one in it is
safe at all. The warnings name rules the same way but counting from one -
`schedule rule 3: ...` - since that is a line for a person to read rather than
an index for the app.

A **ruleset**, by contrast, is addressed by its name and never by its position,
and this is the one place the two differ deliberately. A ruleset's position
moves whenever one above it is added or taken away, so an index a screen read a
minute ago would edit the wrong ruleset; a rule's position within a ruleset
moves only when that ruleset's own rules do, which is the edit the screen is
making. An edit says which ruleset by name and which rule by position inside it,
and an empty name means the flat array.

`[peek] hotkey`, and `hotkey` on a preset, are read as **Qt portable text**:
modifiers spelled `Ctrl`, `Shift`, `Alt` and `Meta`, joined to the key and to
each other with `+`, and a key that is a letter, a digit or a name like `F5`,
`Space` or `Home` - `"Ctrl+Shift+W"`, `"Alt+1"`, `"Meta+Shift+F5"`. **On macOS
`Ctrl` means Command and `Meta` means the physical Control key**, which is Qt's
convention and the one tdesktop's own shortcuts file uses; the Work Mode box
prints each key back in the platform's own spelling.

A preset's `hotkey` turns it on, and turns it off again if it is already
running, so one key covers both directions. Two presets sharing a key is
refused with a warning, and so is a preset taking the peek key: two actions
holding the same sequence make it ambiguous and Qt then fires *neither*, so a
silent duplicate would break both rather than pick a winner. The comparison is a
crude case-and-space normalisation rather than a real `QKeySequence` one,
because this parser is compiled standalone against Qt Core and `QKeySequence` is
QtGui - it catches the same key written twice, not every spelling of it.

None of these are part of tdesktop's own shortcut table - see
[work_mode.md](work_mode.md).

## Implementation

    Telegram/ThirdParty/purple_core/purple/   the submodule, shared verbatim
        purple_settings.{h,cpp}   data model and parser
        purple_splice.{h,cpp}     the surgical writes
        purple_state.{h,cpp}      state.toml
        purple_screentime.{h,cpp} the screen-time log, nothing records it yet
    Telegram/SourceFiles/purple/
        purple_config.{h,cpp}     file IO, watcher, API
        purple_last_seen.{h,cpp}  the reasons and the trade
        purple_readme.{h,cpp}     the generated readme

The Work Mode spec puts the parser and the splice engine inside `purple_config`.
They are split out here for one reason: both are free of every tdesktop
dependency, needing nothing but Qt Core, which lets `purple/test_config.sh`
compile them into a standalone harness and run a few hundred fixture edits in a
second - and is what let them move into a repository of their own, shared with
the Android app. Changes to them are made in purple-core and the submodule
pointer bumped here. Proving that a splice leaves a hand-written file alone
takes many more iterations than a full app build would ever make practical, and
tdesktop has no
unit-test framework in the app target - `cmake/tests.cmake` builds visual GUI
test apps behind `DESKTOP_APP_TEST_APPS`.

Run the tests with:

    purple/test_config.sh

They cover the config half of the spec's acceptance tests, including 100
add/remove cycles that must leave the file byte-identical, canonicalization of a
squashed array, CRLF files, and a display name containing a newline - which
would otherwise write a second line into the members array.

Every splice re-parses what it produced and compares the resulting members
against what was intended before handing it back. A bug there refuses the edit
rather than leaving you a file to repair by hand.

toml++ is vendored as a single header at
`Telegram/ThirdParty/purple_core/tomlplusplus/toml.hpp` (v3.4.0, MIT) rather
than taken from Homebrew, so the packaged build has one less keg that can shadow
or conflict with another - `docs/mac/build.md` covers what that cost us with Qt
and with FFmpeg.
It is compiled with `TOML_EXCEPTIONS 0`, so a malformed file returns an error to
check instead of throwing through a Qt event handler. It is on the include path
of only the three files that need it; putting a 486KB header-only parser in front
of every translation unit would change their compile commands for no reason.

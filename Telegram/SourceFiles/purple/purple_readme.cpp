/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_readme.h"

#include "purple/purple_config.h"
#include "settings.h" // cWorkingDir

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QSaveFile>

namespace Purple {
namespace {

// Replaced with the real path before the file is written, because it is the one
// thing in here nobody can guess and the one thing that differs per install.
constexpr auto kLogToken = "@LOG@";

// The user-facing reference. It is deliberately a document about the file next
// to it rather than about this code: paths, key names and what to do when
// something is wrong, in that order, because that is the order the questions
// arrive in. The design reasoning lives in docs/purple/work_mode.md, which is
// in the repository and not on the user's disk.
constexpr auto kReadme = R"MD(# Purple Telegram

**This file is generated.** The app rewrites it on every launch, so anything
you type into it is lost. It is a reference, not a setting.

What you edit is `settings.toml`, in this same directory. It is reloaded as you
save it - there is no restart and no Apply button.

## The two files

`settings.toml` is yours. The app reads it and makes only the three surgical
writes below, each of which edits the lines it needs and leaves your comments,
blank lines, alignment and key order exactly where they were.

`state.toml` is the app's. It holds the active preset and why it is active, the
focus-sync memory, the schedule's pause flag, the peek timer, and a cache of the
last resolution that worked - so a settings.toml that stops parsing mid-edit
leaves the app running on something known-good instead of on defaults. It is
rewritten constantly, carries no comments and preserves nothing. That is the
entire reason it is a separate file: it must never touch the mtime of the file
you are editing by hand.

## What the app writes for you

- **List membership.** Right-click a chat, then Work Mode, then the list. It
  writes one id into that list's `members` array, with the chat's name in a
  trailing comment. Names go stale, so the comment is regenerated every time the
  line is rewritten rather than read back.
- **The pinned order of an extra view.** Drag a row on one of a preset's own
  tabs and the `pinned` array is rewritten to match.
- **The Premium toggle**, from Settings. It rewrites one value token.

Every one of these re-parses what it produced and checks it against what was
intended before handing it back. A bug there refuses the edit rather than
leaving you a file to repair by hand.

## The model

Two ideas.

- A **list** says who is in it, and nothing else.
- A **preset** names the lists it wants, in order, and says what each one does.

Order is priority *and* capture: walking a preset's `list_order`, the first
entry whose list holds a chat decides that chat, and nothing further down ever
sees it. A chat no entry claims is **hidden and silenced** - a preset names what
gets through, and saying nothing about a chat is a decision too.

Nothing is active until a preset is. Until then the app behaves exactly like
stock Telegram Desktop; that is what the built-in `normal` preset means, and it
is a bypass rather than a permissive preset.

A running preset does not empty your chat list. It replaces the **All chats**
tab with a view of its own, named after the preset, holding what it does not
hide. A hidden chat is still there: still pinned, still searchable, still in the
forward picker. Set `hide_everywhere_p = true` on a preset if you would rather
it were gone from the whole app.

## Lists

```toml
[lists.close_people]
title   = "Close people"
members = [ 1234567890, 5551234567 ]
```

`members` are peer ids. `title` is what the chat menu calls the list; without
one it uses the key. `kinds` matches by what a chat *is* instead:

```toml
[lists.channels]
title = "Channels"
kinds = ["channels"]
```

The four kinds are `private`, `groups`, `channels` and `bots`. A list may carry
both keys and matches a chat that is in either. Both may be empty, which is a
list matching nothing - a fine placeholder to fill from the chat menu later.

Adding a chat from the menu always writes an explicit id, even to a list that
matches by kind. That is how you pull one chat out of a rule that would
otherwise have swept it up somewhere else.

## Presets

```toml
[presets.work]
list_order = [
  { list = "os",           show_mode = "always" },
  { list = "close_people", show_mode = "message_or_reaction" },
  { list = "bots",         show_mode = "never", notify_p = false },
]
```

Optional keys: `default_view_name` renames the tab, `hide_everywhere_p` takes
hidden chats out of the whole app rather than only out of this view, and
`hotkey` binds a key to it.

### hotkey

```toml
[presets.work]
hotkey = "Ctrl+Shift+W"
```

Pressing it turns the preset on. Pressing it while that preset is already
running turns it **off**, back to stock behaviour - so one key means both "get
to work" and "come back", rather than being a no-op half the time.

The string is **Qt portable text**, exactly as `[peek] hotkey` uses it:

- modifiers are spelled `Ctrl`, `Shift`, `Alt` and `Meta`
- they join to the key, and to each other, with `+`
- the key itself is a letter, a digit, or a name like `F5`, `Space`, `Home`
- examples: `"Ctrl+Shift+W"`, `"Alt+1"`, `"Meta+Shift+F5"`

**On macOS `Ctrl` means Command, and `Meta` means the physical Control key.**
That is Qt's convention, not a choice made here, and it is the same one
tdesktop's own shortcuts file uses. The Work Mode box prints each key the way
your keyboard actually has it, so check there if you are unsure.

Two presets on one key is refused with a warning, and so is a preset on the
peek key. That is not tidiness: two actions holding the same sequence make it
ambiguous, and Qt then fires **neither**, so a silent duplicate would break both
keys rather than pick a winner.

These keys are not in Settings > Shortcuts. They belong to this file, along with
everything else about a preset.

Without `default_view_name` the tab is the preset's name with its first letter
capitalised - unless the name already has a capital somewhere, in which case it
is left exactly as written. `work` becomes `Work`, `deep focus` becomes
`Deep focus`, and `iH` stays `iH`.

You cannot name a preset `normal`; that name is the bypass.

### show_mode

When a chat is in the view at all.

- `"always"` - it is there.
- `"message"` - when it has an unread message, or you marked it unread.
- `"message_or_reaction"` - the above, or an unread reaction.
- `"mention"` - only when it names you: a direct mention or a reply.
- `"never"` - it is not.

The three in the middle depend on unread state, so those chats come and go on
their own as messages arrive and are read. A chat you are reading never
disappears from underneath you.

Leaving `show_mode` out takes the default for what the chat *is*:

- a **channel**: `"always"` - you subscribed to it
- a **bot**: `"always"` - you started it
- a **group**: `"mention"` - the noisy ones, so only when they name you
- a **private chat**: `"message"` - when the person has said something

That is why one entry can leave it out and still do the right thing for four
kinds of chat at once, and it is why leaving it out is usually right.

### notify_p

`false` silences the chats an entry claims, on top of your own mute settings.
The preset only ever *adds* a mute, so a chat can be silenced by both, and the
mute menu says which one an Unmute would actually lift. Default true.

### after_close_chat_style

How the chat list marks a row that is only in the view for the moment - inside
its close buffer, or held open by a **Show until**:

- `"none"` - the default. Nothing is drawn.
- `"stripe"` - a bar down the row's left edge.
- `"timer"` - a ring where the unread badge would sit, emptying as it runs out.

```toml
[recent]
stay_visible_after_close = "2m"
after_close_chat_style   = "timer"
```

The ring gives up the badge slot to a count or a pin, which are facts about the
chat rather than about how long the row has left.

## Show until, Hide until, Notify until

Three entries in the `Work Mode` submenu, each offering 30 minutes, 2 hours, 8
hours or 24 hours:

- **Show until** - in the view past the preset's rules. Does not un-silence it.
- **Hide until** - out of the view, and silenced while it is gone.
- **Notify until** - may interrupt you. Lifts only the preset's mute, never a
  mute you set yourself.

`Cancel '... until'` appears while one is running.

Each is scoped to the preset it was made under, so switching preset puts it
aside rather than carrying it along, and switching back brings it out again if
it has not run out. They are kept in `state.toml`, expire by themselves, and
survive a restart.

A hide always leaves the preset's view. How far it goes beyond that is yours:

```toml
[overrides]
hide_scope = "keep_in_folder_but_exclude_from_badge_count"
```

- `keep_in_folder_but_exclude_from_badge_count` - the default. The row stays on
  its folder tabs and keeps its own badge, but stops counting towards theirs.
- `hide_everywhere` - out of the chat list altogether while it lasts, tabs
  included.
- `keep_in_folder` - the row stays and keeps counting.

A peek puts all of it back for as long as you are peeking.

## Making a list from the chat menu

`New list...` at the foot of the `Work Mode` submenu writes an empty table
here, after the last list already in the file:

```toml
[lists.reading]
title = "Reading"
members = [
]
```

and nothing else. **The new list does nothing until a preset names it.** There
is no write in the app that edits a `list_order`, and guessing which preset
wanted the list would be worse than saying so - which the toast does. Tick
chats into it from the same menu right away; add `{ list = "reading" }` to a
preset's `list_order` by hand when you want it to start deciding anything.

The submenu itself only appears once this file defines at least one list, so
the very first one has to be written by hand. Every one after it can come from
the menu.

### pinned

A list of peer ids, and writing it changes who owns the main view's order.

Without it, the preset's main tab **mirrors** your account's pinned chats.
Pinning there pins in the chat list, reaches the server, and stops at the
account's five.

With it, the preset **owns** the order: a pin made inside the preset is written
back here, never travels to the server, and leaves your other devices alone.
That is also what lifts the five-chat limit - five is the server's cap on the
account order the mirror was copying.

```toml
[presets.work]
pinned = [ 1234567890, 987654321 ]
```

Dragging a pinned row inside the preset rewrites this array. Delete the key to
go back to mirroring.

### stories

On a **preset**, what the stories strip does while it runs. Each value hides
strictly more than the one before it:

- `"all"` - no filtering; the strip is stock Telegram.
- `"all_unseen"` - everybody, but only while they have something unseen.
- `"follow"` - the default. Hide whoever the preset excludes outright.
- `"follow_unseen"` - `follow`, and only while unseen.
- `"none"` - no strip at all.

`follow` turns on one distinction. Somebody the preset excludes **outright** -
no list claims them, or one claims them with `show_mode = "never"` - loses
their story. Somebody it admits and is only holding back for being quiet
(`"message"`, `"message_or_reaction"`, `"mention"`) keeps theirs, because a
story *is* new activity, which is what those modes are asking to be shown.

On a **`list_order` entry or a folder**, the same key takes a narrower
vocabulary - `"always"`, `"unseen"`, `"never"` - and speaks only for that
entry's own people. There is no `"all"` there, and writing one warns: an entry
is already a set of people.

```toml
[presets.work]
stories = "follow_unseen"
list_order = [
  { list = "close_people", show_mode = "message_or_reaction",
    stories = "always" },
]
folders = [ { name = "Music", stories = "never" } ]
```

A folder beats an entry, and both beat the preset's policy. A peek reveals
stories along with the chats they belong to.

Your own "add a story" button is governed like anyone else's, so under
`follow` it goes unless a list names you. Put your own id in a list, or write
`stories = "all"`, to keep it.

## Staying visible a little longer

An unread-watching mode has one sharp edge: reading a chat is exactly what
takes it out of the view, so it vanishes on the frame you click away from it.
Having just read something is decent evidence that you are still working on it,
so it can be made to linger:

```toml
[recent]
stay_visible_after_close = "2m"
applies_to = "already_in_view"
```

`stay_visible_after_close` takes the same spellings as `auto_off` above:
`"90s"`, `"2m"`, `"1h"`, a bare number of seconds, or `"off"`. It is `"off"`
unless you write it, and the clock starts when you stop looking at the chat -
not when you open it - so reading something for an hour still buys the full
period afterwards.

`applies_to` decides which chats it covers:

- `"already_in_view"` (the default) - only a chat that was in the view when you
  opened it. The narrow repair: nothing you open can pull in a chat the preset
  was hiding.
- `"any_open_chat"` - any chat you open, hidden or not. One rule, and the only
  one of the three that helps when you reach a hidden chat through search or
  through an extra view: it appears in the main view while you have it open and
  for the period after.
- `"any_open_chat_except_in_folder"` - the above, minus the chats that are
  already one click away in this preset: on an extra view, or in a folder whose
  tab is showing. If it is on P0 already, it does not also need to be in Work.

None of it is remembered across a restart, which is deliberate: a grace period
that survived one would mean the app remembering that you glanced at somebody
yesterday.

## Folders

`folders` names the account's real Telegram folders. **A preset with no
`folders` key shows no folder tabs at all** - write `"*ALL"` to get them back.

```toml
folders = [
  "*ALL",
  { name = "Music", badge_p = false, show_mode = "always",
    include_in_main_view = "pinned" },
]
```

- `enabled_p` - false makes the preset ignore the entry outright: no tab,
  nothing silenced, nothing fed into the view, nothing counted. Default true.
  The comment character you do not have to add and remove, for an entry you are
  keeping around with its settings intact. Not the same as `show_p = false`,
  which only takes the tab off the strip. It also beats `"*ALL"`: a folder
  switched off by hand stays off in a preset that also asks for every folder.
- `show_p` - whether the folder's **tab** is in the strip. The only one of these
  about the tab rather than about the chats. Default true.
- `notify_p` - false silences the folder's chats.
- `badge_p` - false takes the folder out of every count: no number on its own
  tab, and its chats left out of the app badge. For a folder that is background
  noise on purpose, a count is a number you have already decided not to act on.
- `show_mode` - the mode this folder's chats take. Leaving it out gives each
  chat the default for what it is.
- `include_in_main_view` - how much of the folder joins the preset's own view,
  whatever the lists decided:
  - `"none"` (the default) - nothing. Its chats are left to the lists.
  - `"pinned"` - only the chats pinned *inside that folder*.
  - `"all"` - everything in it.

What `include_in_main_view` lets in comes in **even if the chat is archived**.
Archiving is how visibility is controlled in stock Telegram; under a preset the
preset controls it, so a folder that asked for its chats gets them wherever they
happen to be filed. They stay archived - they are simply also in the view.

`"*ALL"` stands for every folder the selection does not name elsewhere, in the
account's own order, at the position you wrote it. So
`folders = [ { name = "B", notify_p = false }, "*ALL" ]` reads as "B on my
terms, then everything else on default terms".

## Extra views

A tab of the preset's own, drawn after its main view and before any folder, with
its own unread badge and its own pinned order.

```toml
[[presets.work.views]]
name       = "P0"
pinned     = [ 1234567890 ]
list_order = [ { list = "close_people" } ]
```

A view's `list_order` selects **membership only**. `show_mode` is read there as
in-or-out: only `"never"` drops a chat, and every other value - including no
value at all - keeps it, unconditionally. That is what a view is for. A chat the
main policy gates behind an unread message can sit permanently on a tab of its
own, so "quiet in the chat list, always one click away" is two lines of config
rather than a compromise.

`notify_p` means nothing on a view and warns, because a chat has one mute state
however many tabs it appears on.

`pinned` is the tab's, not the account's: pinning a chat there does not pin it
in the chat list. A view with no `pinned` sorts by date.

A view may show a chat the main policy hides - unless the preset also says
`hide_everywhere_p = true`, where the chat is out of the chat list entirely and
so cannot be on a tab of it. That combination warns and drops the views.

## Spreads

A bare `"*name"` string inside `list_order` or `folders` splices in a named
sequence, the way Python spreads a list:

```toml
[list_sets.core]
list_order = [
  { list = "os",        show_mode = "always" },
  { list = "emergency", show_mode = "always" },
]

[folder_sets.work_folders]
folders = [ { name = "B" }, { name = "Music", badge_p = false } ]

[presets.work]
list_order = [ "*core", { list = "bots", show_mode = "never" } ]
folders    = [ "*work_folders" ]
```

Sets may spread other sets. This is what replaces preset inheritance: reuse,
without a chain to walk when you are trying to read what a preset actually does.

A name mentioned twice in the same array keeps its **first** mention. That is
forced for a `list_order`, where order is capture, and folders follow the same
rule so there is one to remember rather than two. So writing your own entry
*before* a spread overrides what the spread would have said - which is the idiom
spreads exist for, and the one case where the duplicate is not reported.

## Schedule, focus sync, peek

```toml
[schedule]
enabled_p = true

[[schedule.rules]]
enabled_p = true
days      = ["mon", "tue", "wed", "thu", "fri"]
from      = "09:00"
to        = "17:00"
preset    = "work"

[focus_sync]
enabled_p    = false
enter_preset = "work"
exit_preset  = "previous"

[peek]
hotkey   = "Ctrl+Shift+E"
auto_off = "2m"
```

`days` are `mon` to `sun`, times are `HH:MM` local. A rule whose `to` is earlier
than its `from` spans midnight and is handled as such.

`focus_sync` follows the system Focus mode. `exit_preset = "previous"` puts back
whatever was active when the focus came on, rather than a preset named outright.

A **peek** temporarily reveals what the active preset hides - every chat, every
folder - and deliberately leaves the silencing exactly where it was. It ends on
`auto_off`: `"90s"`, `"2m"`, `"1h"`, or `"off"` to run until you turn it off.

`hotkey` is read as Qt portable text, so **on macOS `Ctrl` means Command** and
`Meta` means the physical Control key. The Work Mode box prints the combination
the way your keyboard actually has it.

## Every boolean key ends in `_p`

A naming convention, not a type distinction. It means the answer is yes or no,
and it makes a flag recognisable as one without looking it up. `show_mode` and
`include_in_main_view` carry no suffix because they are not yes-or-no questions.

## When something is wrong

The file is hand-written, so nearly everything is a warning that leaves usable
settings behind. The Work Mode box lists them all; the log names the line.

- An entry naming a list with no `[lists.x]` table claims nothing, and says so.
  It looks identical to one that is working, which is exactly why it warns.
- A `"*name"` reference to a set that does not exist is dropped. So is one that
  refers back into itself, however many hops it takes.
- A preset that names no list at all is legal and hides everything. Said out
  loud rather than left to be discovered.
- A schedule rule or focus action naming a preset that does not exist is
  skipped. A *disabled* rule is left alone.
- Keys from earlier versions are reported by name, with what to write instead:
  `show`, `notify` and `locked` on a list; `inherit` and `overrides` on a
  preset; `show_p` and `groups_require_mention_p` on an entry; `filtered`,
  `include_in_main_view_p` and `pinned_only_p` on a folder; a top-level
  `list_order`. Silence there would let a file that is doing nothing look like
  one that works - which is exactly what a folder still saying
  `include_in_main_view_p` would do: include nothing.
- A list whose name starts with `*` is refused: nobody reading the file could
  tell it from a spread.

Only one thing is fatal, because it leaves nothing meaningful to run on: a TOML
syntax error. The app keeps the last settings that worked, logs the line and
column, and refuses to write to the file at all - you may be halfway through an
edit, and a blind write would hand you a second mess on top of the first.

## When it is not doing what you meant

Right-click the chat and read the top of the **Work Mode** submenu. It names the
list that claimed the chat and what actually happened to it - "In 'Close people':
hidden until a message" - which is the question that brings anyone here. "In no
list 'Work' names" is as real an answer as any of them.

Then the log, at

    @LOG@

One line per load says what parsed, and one line per change says what it did:

    Purple: 41 of 300 loaded chats never shown, 12 unread-gated (3 showing),
    41 silenced, view holds 262.

A preset that hides nothing looks exactly like a preset that is working, and the
usual cause is a list name spelled slightly wrong.
)MD";

} // namespace

QString ReadmeFilePath() {
	return ConfigDirectory() + u"/readme.md"_q;
}

void RefreshReadme() {
	const auto path = ReadmeFilePath();

	// The one substitution. Everything else in the document is the same for
	// every install, and a template with two moving parts would be a template.
	const auto working = cWorkingDir();
	auto wanted = QString::fromUtf8(kReadme).replace(
		QLatin1String(kLogToken),
		working.isEmpty()
			? u"log.txt, beside the app's own data"_q
			: (working + u"log.txt"_q));

	// Compared rather than written blindly: this runs on every launch, and a
	// write nobody asked for would move the mtime, wake the directory watch and
	// make a file the user never edited look edited.
	auto file = QFile(path);
	if (file.open(QIODevice::ReadOnly)
		&& QString::fromUtf8(file.readAll()) == wanted) {
		return;
	}
	file.close();

	if (!QDir().mkpath(ConfigDirectory())) {
		LOG(("Purple Error: Could not create %1.").arg(ConfigDirectory()));
		return;
	}
	auto out = QSaveFile(path);
	if (!out.open(QIODevice::WriteOnly)) {
		LOG(("Purple Error: Could not write %1.").arg(path));
		return;
	}
	out.write(wanted.toUtf8());
	if (!out.commit()) {
		LOG(("Purple Error: Could not commit %1.").arg(path));
		return;
	}
	LOG(("Purple: wrote %1.").arg(path));
}

} // namespace Purple

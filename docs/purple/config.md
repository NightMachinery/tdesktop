# Fork configuration

Purple Telegram keeps its own settings in plain TOML, outside Telegram's
encrypted `tdata`:

    $XDG_CONFIG_HOME/purple-telegram/settings.toml
    $XDG_CONFIG_HOME/purple-telegram/state.toml

falling back to `~/.purple-telegram/` when `XDG_CONFIG_HOME` is unset.
`settings.toml` is created with commented defaults on first run.

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

`settings.toml` is yours. The app reads it and makes exactly two kinds of write
to it, both surgical:

- the Premium toggle in Settings, which rewrites the single value token of
  `[premium] enabled_p`,
- adding and removing list members, which edits one line of a `members` array.
  That is the `Work Mode` submenu on a chat's context menu - see
  [work_mode.md](work_mode.md).

Neither re-serializes the document. Both locate what they need through toml++
source regions and edit the raw lines, so comments, blank lines, alignment and
key order all survive. `enabled   =    true   # keep ads away` comes back as
`enabled   =    false   # keep ads away`, spacing and comment intact.

`state.toml` is the app's. It holds the active preset and why it is active, the
focus-sync memory, the schedule's pause flag and last target, and the peek
timer. It is rewritten
whenever any of that changes, carries no comments, and preserves nothing. That
is the entire reason it is a separate file: state churns constantly, and it must
never touch the mtime of the file you are editing by hand.

It is reloaded live too, which is not something a machine-owned file needs but
keeps it hand-editable now that the preset box is a second writer to the same
fields - see [work_mode.md](work_mode.md). The app's own writes come back
through the same watch, compare equal to what it just wrote, and do nothing.

Both are written through `QSaveFile`, which writes a temporary alongside the
target and renames over it, so a crash mid-write cannot leave a truncated config.

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

Only one thing is fatal, because it leaves nothing meaningful to run on: a TOML
syntax error. The app keeps the last settings that worked, logs the line and
column, and refuses to write to the file at all - you may be halfway through an
edit, and a blind write would leave you with a mess to untangle on top of
whatever you were already fixing.

Everything the app could not make sense of is available to the UI as a warning
list, for the banner described in the Work Mode spec.

## Schema

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

`folders` names the account's real folders, each with `show_p` (its **tab** is in
the strip - the only one of the four about the tab rather than the chats),
`notify_p` (false silences its chats), `badge_p` (false takes the folder out of
every count - no number on its tab, its chats out of the app badge), `show_mode`
(the mode its chats take, on the same terms as `notify_p`; leaving it out gives
each chat the default for what it is) and `include_in_main_view`,
which is `"none"` (the default), `"pinned"` or `"all"` - how much of the folder
joins the preset's own view whatever the lists decided. What it lets in comes in
even if the chat is archived, since under a preset the preset is what controls
visibility. A preset with no `folders` key shows no folder tabs; `"*ALL"` is
every folder you have.

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

The rest - `[schedule]`, `[focus_sync]`, `[peek]` - is documented by the starter
file the app writes on first run, which carries a commented example of each.

`[peek] hotkey` is read as Qt portable text, so on macOS `Ctrl` means Command
and `Meta` means the physical Control key. It is deliberately not part of
tdesktop's own shortcut table - see [work_mode.md](work_mode.md).

## Implementation

    Telegram/SourceFiles/purple/purple_settings.{h,cpp}   data model and parser
    Telegram/SourceFiles/purple/purple_splice.{h,cpp}     the surgical writes
    Telegram/SourceFiles/purple/purple_state.{h,cpp}      state.toml
    Telegram/SourceFiles/purple/purple_config.{h,cpp}     file IO, watcher, API

The Work Mode spec puts the parser and the splice engine inside `purple_config`.
They are split out here for one reason: both are free of every tdesktop
dependency beyond `base/` and Qt Core, which lets `purple/test_config.sh`
compile them into a standalone harness and run a few hundred fixture edits in a
second. Proving that a splice leaves a hand-written file alone takes many more
iterations than a full app build would ever make practical, and tdesktop has no
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
`Telegram/ThirdParty/tomlplusplus/toml.hpp` (v3.4.0, MIT) rather than taken from
Homebrew, so the packaged build has one less keg that can shadow or conflict with
another - `docs/mac/build.md` covers what that cost us with Qt and with FFmpeg.
It is compiled with `TOML_EXCEPTIONS 0`, so a malformed file returns an error to
check instead of throwing through a Qt event handler. It is on the include path
of only the three files that need it; putting a 486KB header-only parser in front
of every translation unit would change their compile commands for no reason.

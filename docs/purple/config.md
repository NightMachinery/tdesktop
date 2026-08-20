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
  `[premium] enabled`,
- adding and removing list members, which edits one line of a `members` array.

Neither re-serializes the document. Both locate what they need through toml++
source regions and edit the raw lines, so comments, blank lines, alignment and
key order all survive. `enabled   =    true   # keep ads away` comes back as
`enabled   =    false   # keep ads away`, spacing and comment intact.

`state.toml` is the app's. It holds the active preset and why it is active, the
focus-sync memory, the schedule pause flag and the peek timer. It is rewritten
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

- A missing catch-all list is synthesized. All four of `@private`, `@groups`,
  `@channels` and `@bots` always exist, which is what lets the rest of the
  engine skip an "unlisted chat" case entirely.
- `list_order` is reconciled: names with no list are dropped, lists it forgot are
  appended at the bottom, and the catch-alls are forced below your own lists
  whatever it says. Reordering the catch-alls among themselves is respected.
- An override of a list that does not exist, or of a locked one, is dropped.
- A preset named `default` or `normal` is refused; those names are reserved.
- A preset inheriting from something that does not exist falls back to
  `default`.
- A schedule rule or focus-sync action naming a preset that does not exist is
  skipped.
- Duplicate member ids are deduplicated on read, keeping the order you wrote.

Only two things are fatal, because neither leaves anything meaningful to run on:
a TOML syntax error, and a loop in preset inheritance. In both cases the app
keeps the last settings that worked, logs the line and column, and refuses to
write to the file at all - you may be halfway through an edit, and a blind write
would leave you with a mess to untangle on top of whatever you were already
fixing.

Everything the app could not make sense of is available to the UI as a warning
list, for the banner described in the Work Mode spec.

## Schema

`[premium] enabled` controls the client-side Premium unlocks; see
[premium.md](premium.md) for what that covers and what it deliberately does not.

The Work Mode half - lists, presets, folders, schedule, focus sync and peek - is
documented by the starter file the app writes on first run, which carries a
commented example of each section. The parser reads the whole schema today;
`[schedule]` and `[focus_sync]` are the two sections nothing consumes yet.

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

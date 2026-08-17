# Fork configuration

Purple Telegram keeps its own settings in plain TOML, outside Telegram's
encrypted `tdata`:

    $XDG_CONFIG_HOME/purple-telegram/settings.toml

falling back to `~/.purple-telegram/settings.toml` when `XDG_CONFIG_HOME` is
unset. The file is created with commented defaults on first run.

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

## The file is yours

The app treats `settings.toml` as hand-owned. Comments and formatting are
preserved: writing a value rewrites exactly the one token it is responsible for,
found through toml++ source regions, and never re-serializes the document. Writes
go through `QSaveFile`, so a crash mid-write cannot leave a truncated file.

If the file does not parse, the app logs the line and column, keeps its defaults
in memory, and refuses to write to it at all - you may be halfway through an
edit, and a blind append would leave you with a duplicate table to untangle.

Changes made by hand are picked up on the next start. There is no file watcher
yet.

## Current schema

    [premium]
    enabled = true

See [premium.md](premium.md) for what that unlocks.

## Implementation

`Telegram/SourceFiles/purple/purple_config.{h,cpp}`.

toml++ is vendored as a single header at
`Telegram/ThirdParty/tomlplusplus/toml.hpp` (v3.4.0, MIT) rather than taken from
Homebrew, so the packaged build has one less keg that can shadow or conflict with
another - `docs/mac/build.md` covers what that cost us with Qt. It is compiled
with `TOML_EXCEPTIONS 0`, so a malformed file returns an error to check instead
of throwing through a Qt event handler.

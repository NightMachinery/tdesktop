# Purple agent and feature guide

Read this guide before doing any Purple work. It collects the Purple-specific
project requirements and feature documentation referenced by AGENTS.md and README.md.

## External test tooling

Private build-machine notes live in the private purple-build repo; when it is cloned at ~/.purple-build, see ~/.purple-build/readme.md.

## Build and test

### Build scheduling

Full Purple Telegram desktop and Android application builds may run whenever
the user explicitly authorizes the build and grants access to the machine.
Night scheduling is an option when the user requests it, not a standing time
restriction. Do not leave an active agent waiting for a requested window to
open. The short `purple/test_config.sh` core harness and non-building static
checks may run at any time.

### Build commands and release configuration

Use these Debug commands for development and verification builds. They are not
the macOS installation workflow for the Purple fork. See the macOS rules below
before replacing `/Applications/Purple Telegram.app`.

Never build the Purple Telegram application in the literal CMake `Release`
configuration. It is extremely heavy and is not the packaged configuration
used by this fork. This does not apply to dependency builds whose documented
workflow explicitly requires `Release`, such as the patched Qt build.

### macOS build and install rules

For the Purple fork, distinguish a test build from the installed daily-use
client:

- Development and disposable test bundles use `Debug`.
- `/Applications/Purple Telegram.app` must be built with the default
  `RelWithDebInfo` configuration from `purple/build_app.sh`, then installed
  with `purple/install.sh`. Do not pass `BuildType=Debug` for this target.
- Do not use literal CMake `Release` for the Purple Telegram application;
  `RelWithDebInfo` is the optimized, symbolicated configuration intended for
  an installed Purple client.
- A non-Windows `_DEBUG` build first tries `cExeDir()` as its working directory.
  If an installed app bundle is writable, that can create a fresh `tdata`
  beside the executable and make the app appear logged out even though the
  established account remains under
  `~/Library/Application Support/Purple Telegram/tdata`. Never install a Debug
  bundle over the daily-use Purple app. Give Debug builds a separate
  `BuildPath`, install them to a separate `Target`, pass that same `BuildPath`
  to `purple/install.sh`, and launch them with an explicit isolated `-workdir`.

## Best Practices

1. Use Debug for ordinary development and verification.
2. Use `RelWithDebInfo` for the installed macOS Purple client.
3. Never build the Purple Telegram application in the literal CMake `Release`
   configuration; follow documented configuration requirements for dependency
   builds.

## Quitting the fork on macOS: never by name

Several Telegram clients are usually installed side by side, and their names
collide:

    /Applications/Purple Telegram.app   com.tdesktop.PurpleTelegram
    /Applications/Telegram.app          com.tdesktop.Telegram
    /Applications/Telegram_macOS.app    ru.keepcoder.Telegram

`osascript -e 'quit app "Purple Telegram"'` resolves the name through
LaunchServices rather than targeting one bundle, and has quit the user's real
Telegram instead of the fork. Losing someone's messaging client mid-conversation
to a build step is not an acceptable side effect of installing.

Target the bundle identifier, which is unambiguous:

```bash
osascript -e 'tell application id "com.tdesktop.PurpleTelegram" to quit'
```

Or signal explicit pids, matched against the full executable path rather than a
process name:

```bash
Binary="/Applications/Purple Telegram.app/Contents/MacOS/Purple Telegram"
ps -Ao pid=,comm= | awk -v b="$Binary" '$0 ~ b { print $1 }'
```

Do not use `killall Telegram`, `pkill Telegram`, or any `-f` pattern containing
"Telegram": the first two match by short name across every client, and `-f`
additionally matches the shell running the command itself.

Only quit what the current task started. `install.sh` replaces the bundle at
`/Applications`, so it does need the fork closed - ask the user to quit it, or
quit it by bundle id if they have said you may. Never quit a client you did not
launch on the assumption that it is "probably the fork".

## The Purple fork

Everything this fork adds beyond upstream lives under `Purple::` and is findable
with `git grep Purple::`. Read the docs before changing any of it:

- **`roadmap.md`** - what the Android port has and what is left
- **`../mac/build.md`** - the packaged macOS build, install and symbolication
- **`premium.md`** - the client-side-only Premium unlocks
- **`defaults.md`** - the values this fork decides differently from
  upstream before anybody configures anything
- **`config.md`** - `settings.toml`, `state.toml`, live reload
- **`work_mode.md`** - presets, lists, visibility and notify gating
- **`sync.md`** - moving `settings.toml` between installs through
  Saved Messages, and the alternatives that were not built
- **`version.md`** - the Purple version line in Settings and the
  rule for bumping its number
- **`upstream_hooks.md`** - the hook registry: every Purple edit
  in an upstream file, the refactor batch that moves it, and how often
  upstream edits next to it
- **`../remote-build-and-test/readme.md`** - the private build-notes entry point

The parser, the splice engine, the state store and the resolution engine are the
Work Mode core, and they live in the `purple-core` submodule at
`Telegram/ThirdParty/purple_core` so the Android app compiles the very same
sources. Only the platform layer around them is in
`Telegram/SourceFiles/purple/`. Schema and core changes are made in the
purple-core repository, committed and pushed there, and the submodule pointer
bumped here.

`purple/test_config.sh` compiles that core into a standalone harness and runs it
in about a second. Run it after touching the core or anything in
`Telegram/SourceFiles/purple/`; a full app build to test a config change is
never necessary.

## Feature overview

This Purple fork adds a [pinned music cache action](pinned_music.md)
to the open-chat menu.

This checkout also carries a local macOS fork build, "Purple Telegram", which
installs alongside the official app for development: [docs/mac/build.md](../mac/build.md).
The installed daily-use bundle is `RelWithDebInfo`; Debug bundles use separate
build paths, install targets, and working directories as described in the
macOS instructions.
It unlocks the Premium features that are gated on the client alone
([docs/purple/premium.md](premium.md)), adds Work Mode - named presets that
decide which chats you can see and which may interrupt you
([docs/purple/work_mode.md](work_mode.md)) - and keeps its own settings in
TOML ([docs/purple/config.md](config.md)). Its other fork defaults and the
automatic text-replacement preferences are documented in
[docs/purple/defaults.md](defaults.md). Local passcodes also accept the
English key sequence entered with a Persian keyboard layout, as described in
[docs/purple/passcode.md](passcode.md). Desktop notification preview
exceptions for selected chats are configured in
[docs/purple/config.md](config.md). Account-backed settings sync and future
playlist sync have shared-core and local-state foundations but are not yet
exposed in the clients; see the
[sync plan](account_sync_plan.md). Current requests and follow-up work are
tracked in [docs/purple/backlog.md](backlog.md).

## The required hook rule

The upstream hook rule remains in
[AGENTS.md](../../AGENTS.md#keeping-upstream-merges-easy), including its
source-registration, string-registration, and hook-registry requirements.

Message copying and Markdown export are documented in [copy_markdown.md](copy_markdown.md).
Local saving and screenshot behavior are documented in [user_control.md](user_control.md).

The encrypted account archive adapter is described in [archive_storage.md](archive_storage.md).

Sync-chat setup and its native qualification boundary are described in [sync_chat_setup.md](sync_chat_setup.md).

Local emoji artwork export is described in [emoji_export.md](emoji_export.md).

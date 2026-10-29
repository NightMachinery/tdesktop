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

### Running every host gate

Before delivering a patch, run the full host gate set on the exact base you
deliver against, and again after any rebase:

```bash
python3 purple/run_host_gates.py --tree <tree> --build-dir <Debug build> \
    --output-dir <dir outside the tree> --compile-wrapper '<hold wrapper>'
```

`purple/host_gates.json` is the canonical list: every static route check and
compiled host test, with each variant. A patch that adds, renames or removes a
driver, a variant, a `purple/test_*.cpp` fixture or a program under
`Telegram/SourceFiles/purple/tests/` updates that list in the same commit. A
program no driver compiles yet is recorded under `undriven` with the reason. The
runner refuses to start while any driver or program is missing from the list,
while a listed file does not exist, or while a configuration of a driver with
several lists fixtures other than the ones its arguments select (a file name in
its arguments, or a flag mapped under `variants`). `--list` prints the list and
runs only that check.

`purple/test_cases.py --case <name>` builds and runs one program under
`Telegram/SourceFiles/purple/tests/` from the Debug build's recipes: the
production units it needs, the tree's `purple_core` where it uses the core, and
the build's own objects and libraries for the rest. The WebP cases compile a few
production functions on their own (sliced from their files at run time), and a
small Qt program writes a synthetic 512 by 512 WebP and PNG for them, so no
artwork is checked in. Before compiling `webp_paste_cases.mm` the driver checks
that it still uses only a pasteboard it creates by unique name, under a
`QCoreApplication`.

The compiled gates reuse the recipes and objects of an existing Debug build, which
may come from another checkout. The runner keeps that checkout's include roots,
because the build's precompiled header was made from them, and puts in front of
each root an overlay holding the tree's new or changed files there, plus an
`#error` stub for every header the tree deleted. The source each recipe compiles
is always the tree's. The tree needs the `purple_core` submodule populated.

A configuration is STALE, not PASS, when anything it compiled or reused may not
match the tree. Three checks decide it:

- Every compile leaves a dependency file (the runner adds `-MD -MF` where the
  driver does not ask for one). A file read from the build's checkout whose tree
  copy differs or is gone makes the gate STALE. That covers what the overlays
  cannot intercept: a header included from its own directory, a root without an
  overlay, and the precompiled header.
- For every object, archive member and precompiled header the gate linked or
  force-included, and every generated header it read, ninja is asked whether it
  would rebuild it: a dry run (`-n -d explain`) from a copy of `build.ninja`
  outside the build, with no regeneration edge and without the order-only
  dependencies that only order a first build. Any planned step makes the gate
  STALE, and `results.json` lists the steps and ninja's explanations. This is
  ninja's own answer, so a generated header that is behind its `.strings`,
  `.style`, `.palette` or `.tl` source counts too. Only ninja's exact
  "no work to do" is clean: the status format is pinned (`NINJA_STATUS`), and a
  dry run that fails or prints anything of another shape fails the gate. A
  manifest behind its CMake inputs makes every gate that reuses the build STALE.
  The runner learns that from a dry run of `build.ninja` in the same copy, whose
  regeneration edge has lost the `CONFIGURE_DEPENDS` glob check that is dirty on
  every run, so ninja compares the CMake files themselves. A glob that has gained
  files is still visible only to a real build.
- The same reused targets are compared with the tree by content: an input the
  tree changes or deletes, a missing or out-of-date dependency record, or an
  output older than an input. Inputs inside a submodule the tree leaves
  unpopulated count as changed when the build's copy is not at the tree's
  gitlink or has local changes. Changed generator inputs in the tree make every
  gate that reuses the build STALE.

A configuration that reuses the output of a STALE one is STALE too. Refresh the
build and run again; `--accept-stale` counts STALE configurations as passing,
but the summary then starts with `STALE ACCEPTED` and names them.

Compiles run one at a time. `--compile-wrapper` is a command prefix around each
compiler or linker call, for example a script that takes a short
`compiler:local` hold, runs `"$@"` and releases it. A compiling gate that makes
no call through it fails. The gates that do not compile run in parallel
(`--jobs`). `--only` and `--skip` select configurations; `--only` also runs
whatever a selected configuration reuses.

The output directory receives `results.json`, with the list's sha256 and length,
the source tree hash, every submodule's state, and the command, exit code,
duration, log path, reused build targets and stale reasons of every
configuration, plus one log per gate. The tree hash is the one
`git add -A && git write-tree` would give, computed without writing to the
repository. Like write-tree, it records a submodule only by its commit, so the
runner also hashes each populated submodule's files as a git tree (`coreHash` for
`purple_core`) and compares that with the tree the gitlink names; a submodule
whose files differ from its gitlink fails the run. Both hashes are taken again at
the end, and a change fails the run too.

The last line is `N/M PASS` followed by the count of every other status, the
number of programs recorded as undriven and the list's hash, where `M` is always
the length of the canonical list. A run that is not clean says so first: `PARTIAL`
for a run of fewer configurations, `STALE ACCEPTED`, and `NOT CLEAN` when a
submodule is not at its gitlink, the tree changed during the run, or the run
stopped. The exit status is 0 only for a complete run in which everything passed
with nothing stale, so a clean run is exit 0 with a last line starting
`M/M PASS`; 2 when everything passed only because of `--accept-stale`, 3 for a
partial run in which every selected configuration passed, and 1 otherwise.

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

Marked chooser WebP paste is documented in [webp_paste.md](webp_paste.md).

The opt-in local control socket and standalone Go command are documented in
[control_socket.md](control_socket.md).

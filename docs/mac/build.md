## Building Purple Telegram on macOS

This documents the local "Purple Telegram" build: a rebranded Telegram Desktop
that installs and runs alongside the official app, so you can develop against
your own client without giving up the released one.

For the upstream, fully self-contained build see [building-mac.md](../building-mac.md).
This file only covers what differs from it.

### Which build route, and why not the upstream one

Upstream's route (`Telegram/build/prepare/mac.sh`) compiles every dependency
from source — Qt, FFmpeg, OpenSSL and roughly forty others, as universal
x86_64 + arm64 static libraries. That is what a shippable, notarized release
needs, and it costs about 55 GB and the better part of a day.

There is a second route that upstream also maintains, in
`.github/workflows/mac_packaged.yml`: take the dependencies from Homebrew and
build only what Homebrew does not carry. `cmake/validate_special_target.cmake`
selects it automatically when no `Libraries` directory sits next to the
checkout:

```cmake
get_filename_component(libs_loc "../Libraries" REALPATH)
cmake_dependent_option(DESKTOP_APP_USE_PACKAGED "..." OFF libs_loc_exists ON)
```

Homebrew's `qtbase` happens to be 6.11.1, exactly the version
`Telegram/build/qt_version.py` pins, so Qt need not be built at all. That
brings the whole thing down to roughly 12 GB and a few hours, nearly all of it
the Telegram target itself. Qt is in fact built here after all — for its
patches, not its version — but in Homebrew's shape rather than upstream's,
which costs minutes rather than hours; see "Qt with upstream's patches".

The one thing this route gives up is a self-contained universal binary. The
result is arm64-only and leans on Homebrew, which is the right trade for a
development machine and the wrong one for distribution.

Because of the `../Libraries` check above, the extra dependencies must **not**
be installed into a directory of that name beside the checkout, or packaged
mode silently switches off and the build starts looking for libraries that
were never built. This setup uses `../tdesktop-libs` instead.

### Prerequisites

Xcode, selected as the active developer directory:

```bash
sudo xcode-select -s /Applications/Xcode.app/Contents/Developer
```

Then the Homebrew formulae, matching the CI list in `mac_packaged.yml`:

```bash
brew install autoconf automake boost cmake libtool ninja pkg-config \
    ada-url ffmpeg@6 jpeg-xl libavif libheif minizip openal-soft openh264 \
    openssl opus qtbase qtimageformats qtshadertools qtsvg xz
```

CI also runs `brew upgrade` and installs `python`; neither is necessary here,
and both touch more of the system than this build needs.

### API credentials

The build refuses to configure without them — see
`Telegram/cmake/telegram_options.cmake` and
[api_credentials.md](../api_credentials.md) for how to obtain a pair.

They are passed through the environment rather than written into any tracked
file, because `origin` here is a public fork that gets pushed to (upstream
`telegramdesktop/tdesktop` is the `upstream` remote):

```bash
export TDESKTOP_API_ID=... TDESKTOP_API_HASH=...
```

Keeping them in a file outside the checkout and sourcing it works too. Note
that whichever pair you configure with is compiled into the binary, and
changing it later recompiles the whole Telegram target.

### One-time: the dependencies Homebrew does not carry

`rnnoise`, `tg_owt` (the WebRTC fork) and `tde2e`:

```bash
purple/build_deps.sh
```

This mirrors the corresponding CI steps and installs into
`../tdesktop-libs/local/`. It skips anything already installed there, so it is
safe to re-run. `tg_owt` is the slow one, at well over a thousand objects.

The installed prefixes come to about 190 MB. The clone and build trees the
script leaves behind in `../tdesktop-libs/` are another 700 MB or so and are
not needed once the installs succeed; delete them to reclaim the space, at the
cost of a full re-clone if you ever rebuild a dependency.

`build_deps.sh` has no job cap of its own. The `tg_owt` step is a
`cmake --build`, so `CMAKE_BUILD_PARALLEL_LEVEL=4` in the environment is
enough to keep it off every core.

#### When Homebrew upgrades abseil

`tg_owt` is a static library, and abseil puts its release date in an inline
namespace, so every symbol `tg_owt` compiled against abseil carries the
version it was built with. Upgrade the keg and the app is compiled against
`absl::lts_20260526` while `libtg_owt.a` still refers to `absl::lts_20250814`;
nothing reconciles the two. The upgrade is easy to miss because abseil is
rarely what you asked for — it comes in under `protobuf`, so installing or
reinstalling something unrelated is enough to bump it.

It surfaces in two stages. First `ninja` in `out/` fails without compiling
anything, on a library that is simply gone:

```
ninja: error: '/opt/homebrew/lib/libabsl_flags_parse.2508.0.0.dylib', needed by
'Purple Telegram.app/Contents/MacOS/Purple Telegram', missing and no known
rule to make it
```

Re-running `cmake .` in `out/` picks up the new keg and gets the whole tree
compiling again, and then the link fails on the disagreement itself:

```
rtc::AsyncPacketSocket::RegisterReceivedPacketCallback(absl::lts_20260526::AnyInvocable<...>)
absl::lts_20250814::base_internal::ThrowStdOutOfRange(char const*), referenced from ... libtg_owt.a(video_encoder.cc.o)
```

Pointing the build back at the old keg is not a fix. Its dylibs still carry
the install name `/opt/homebrew/opt/abseil/lib/libabsl_*.2508.0.0.dylib`, and
that symlink now resolves to the new keg, where those files do not exist — so
the app would link and then fail to start. `brew cleanup` deletes the old keg
outright.

The fix is to rebuild the library against what is installed now. Move the old
prefix aside rather than deleting it, so it can go back if the rebuild fails:

```bash
mv ../tdesktop-libs/local/tg_owt ../tdesktop-libs/local/tg_owt.abseil-2508
CMAKE_BUILD_PARALLEL_LEVEL=4 purple/build_deps.sh
```

The script skips the dependencies that are still installed and rebuilds only
this one, from a fresh clone. Then re-run `cmake .` in `out/` if the config
changed, and relink. Once the app links, the set-aside prefix and the new
clone tree under `../tdesktop-libs/` are both disposable.

### Configure and build

```bash
. ../tdesktop-libs/api_credentials.sh    # or export the two variables
purple/build_app.sh
```

That script both configures and builds — it is not a configure-only step.
It wraps a normal CMake invocation: Ninja, `RelWithDebInfo`, the Homebrew and
`../tdesktop-libs/local` prefixes on `CMAKE_PREFIX_PATH`, and a build tree in
`out/`. Override `BuildType=Debug` in the environment if you want the `-O0`
build instead, and `BuildJobs=N` to cap parallelism — the default saturates
every core, which makes the machine unpleasant to use for the couple of hours
this takes.

Never run two builds against `out/` at once. Ninja does not lock the build
directory, so a second one silently competes for the same outputs.

`RelWithDebInfo` is the useful default for a client you both use and modify:
optimized enough to live in the Dock, but with enough symbols to say where a
crash landed in code you are changing.

### How much debug info

The debug info is deliberately line tables only. `build_app.sh` overrides
`CMAKE_<LANG>_FLAGS_RELWITHDEBINFO` to `-O2 -gline-tables-only -DNDEBUG`, and it
has to override the per-config flags rather than join `CompilerFlags`: CMake
emits `CMAKE_<LANG>_FLAGS` first and `CMAKE_<LANG>_FLAGS_<CONFIG>` after it, so
a `-gline-tables-only` appended to the former loses to the config's own `-g`
and the change is silently a no-op.

Full `-g` costs far more than it looks. It put 122MB of DWARF into
`history_widget.cpp.o` alone - 90% of that one object, 76MB of it
`__debug_str` - and 17GB across `out/`, which is most of what a configured
build tree here weighs. Almost none of it was reachable: line tables keep both
things this fork actually symbolicates with, function names in the binary and
file:line through the debug map (see "Symbolicating a crash"). What they drop
is variable and type information, so a debugger still gives a correct
backtrace but cannot print a local.

If you need the full thing, reconfigure with

```bash
RelWithDebInfoFlags='-O2 -g -DNDEBUG' purple/build_app.sh
```

and pay one full rebuild, because a flag change invalidates every translation
unit. The recorded ninja timings put that at 23.3 hours of compile time, about
three hours of wall clock on eight cores.

Two flags are worth knowing about:

- `-D DESKTOP_APP_DISABLE_SWIFT6=ON` is required on Xcode older than 16.
  `Telegram/lib_translate/CMakeLists.txt` sets `CMAKE_Swift_LANGUAGE_VERSION 6`
  and links the `Translation` framework, which needs the macOS 15 SDK. The
  flag drops on-device translation, which cannot work below macOS 15 anyway.
- Auto-update is already off. `cmake/variables.cmake` defaults
  `DESKTOP_APP_DISABLE_AUTOUPDATE` to on when `DESKTOP_APP_SPECIAL_TARGET` is
  empty, which is what this build is. That matters more than it sounds: an
  enabled updater would eventually fetch an official build and overwrite the
  rebranded one.

### Xcode 15 and parenthesized aggregate initialization

Apple clang 15, the newest compiler that runs on macOS 14.4 and below, does
not implement P0960 — initialising an aggregate with parentheses,
`TextWithEntities(text, entities)`, a C++20 feature that arrived in upstream
Clang 16. Current Telegram Desktop sources use it. The failures look like

    no member named 'Orientation' in 'QAccessible::Attribute'
    no matching constructor for initialization of 'TextWithEntities'

and appear one file at a time, since each only surfaces when that translation
unit compiles. `ninja -k 0` builds past failures and collects them all in one
pass, which is much faster than fixing them one rebuild at a time.

There turned out to be only three, patched in place to brace initialization:

- `SourceFiles/api/api_transcribes.cpp`
- `SourceFiles/info/profile/info_profile_top_bar.cpp`
- `SourceFiles/boxes/url_auth_box.cpp`, where the argument was
  `make_state<SwitchAccountResult>(nullptr)` — the member it set already
  defaults to `nullptr`, so dropping the argument is equivalent.

These are local patches against upstream and will come back as conflicts or
as fresh call sites whenever you pull. On a machine with Xcode 16 or newer
they are unnecessary — the real fix is a newer toolchain, which needs
macOS 14.5+.

### A system libfido2 older than 1.14

`Telegram/cmake/lib_fido2.cmake` prefers a system libfido2 over the copy this
repository vendors as a submodule. Anything below 1.14 lacks
`fido_assert_authdata_raw_ptr` and `fido_assert_authdata_raw_len`, which
`SourceFiles/webauthn/webauthn_common.cpp` calls, so the build fails late with
undeclared identifiers.

`build_app.sh` passes `-D TDESKTOP_VENDORED_FIDO2=ON` to ignore the system
copy and build the vendored 1.17.0 instead. That is also what upstream's
packaged CI does, since it never installs libfido2 at all.

Upgrading the Homebrew formula would work too, but `openssh` links against it,
so the vendored route avoids touching anything outside the build.

### If Homebrew's full "qt" formula is also installed

This is the merged-prefix route, which `purple/build_qt.sh` has largely
retired — a Qt built into a prefix of its own has none of these clashes, and
the notes below explain why several of its arguments look the way they do.
It still applies whenever you go back to Homebrew's Qt.

It is worth understanding before changing anything in the scripts, because
the symptom looks nothing like the cause. The build fails compiling
`Telegram/lib_ui/ui/accessible/ui_accessible_widget.cpp` with

    no member named 'Orientation' in 'QAccessible::Attribute'

which is a Qt that is too old — `Orientation` arrived after 6.9. It happens
when the `qt` formula (a full Qt, currently 6.9.2, pulled in by gnuplot,
octave and pyqt among others) is installed next to `qtbase`. Homebrew can only
link one of them into `/opt/homebrew`, `qt` wins, and `qtbase` and its
companion modules sit unlinked in prefixes of their own. Upstream CI never
sees this because it installs only the four `qt*` module formulae.

`build_app.sh` handles it without touching Homebrew's links, so the system
`qt` keeps working for everything else. Three separate things were needed:

- CMake finds the wrong Qt, because `/opt/homebrew/lib/cmake/Qt6` belongs to
  `qt`. `merge_qt_prefix.py` builds a merged prefix of symlinks in
  `../tdesktop-libs/local/qt`, reproducing the single-prefix layout that
  Homebrew would have produced, and that goes first on `CMAKE_PREFIX_PATH`.
  A merge is necessary rather than just pointing at `qtbase`: Qt6Config looks
  for its components beside itself, so `qtsvg` and `qtshadertools` have to
  appear under the same prefix.
- Even with CMake resolving 6.11.1, the compiler still reads 6.9.2 headers.
  `/opt/homebrew/include` holds `QtGui`, `QtCore` ... symlinks into `qt`, it
  reaches the command line as `-isystem`, and clang searches every `-isystem`
  directory before *any* framework directory — regardless of the order they
  appear in. Qt's own headers live in frameworks, so they lose.
- The fix is `-F`, which lands in the earlier angled search list. Pointing it
  at Qt's framework directory does nothing, though: clang resolves search
  paths to real directories and drops the duplicate of the `-iframework`
  entry CMake already emitted. `merge_qt_prefix.py` therefore also creates
  `../tdesktop-libs/local/qt/frameworks`, a separate real directory of
  symlinks to the same frameworks, and that is what `-F` points at.

The same clash bites once more at deploy time, with a different symptom: the
app builds, starts, and then aborts with

    This application failed to start because no Qt platform plugin could be
    initialized

and, under `QT_DEBUG_PLUGINS=1`,
`Ignoring QPA plugin due to mismatching Qt versions 396032 395520`
(0x60B00 = 6.11.0 against 0x60900 = 6.9.0). Homebrew builds qtbase with
`/opt/homebrew` as its prefix, so `QLibraryInfo` — and therefore macdeployqt —
looks for plugins in `/opt/homebrew/share/qt/plugins`, which the `qt` formula
owns. The bundle ends up with a 6.9.2 `libqcocoa.dylib` inside a 6.11.1 app.

`merge_qt_prefix.py` handles this by putting a real copy of macdeployqt in the
merged prefix's `bin` (a symlink would resolve back to the keg and read the
wrong qt.conf) next to a `qt.conf` pointing at the merged prefix, and
`install.sh` runs that copy. Verify with:

```bash
otool -L "/Applications/Purple Telegram.app/Contents/PlugIns/platforms/libqcocoa.dylib" | grep QtCore
```

which should report `current version 6.11.1`.

`merge_qt_prefix.py` additionally drops `WrapVulkanHeaders` from Qt6::Gui's
interface. Homebrew's qtbase records the Vulkan headers as living in
`/opt/homebrew/include`, which put that directory in the include path of
every Qt-using target. It is not the only source of that directory, so it is
not sufficient on its own, but Telegram Desktop renders through Metal and
OpenGL on macOS and never needs it.

None of this applies on a machine without the `qt` formula; the merged prefix
is harmless there. None of it applies to `../tdesktop-libs/qt-patched` either:
that prefix is a real Qt installation of its own, its `macdeployqt` already
reports its own plugin directory, and it is configured `-DFEATURE_vulkan=OFF`
so there is no Vulkan include path to remove.

### If Homebrew's full "ffmpeg" formula is also installed

The same shadowing problem, with a much worse failure mode: it builds and links
cleanly and then corrupts memory at runtime.

We link `ffmpeg@6` (libavcodec 60), which is what upstream's packaged CI pins.
A machine that also has the full `ffmpeg` formula has *its* headers linked into
`/opt/homebrew/include` — libavcodec 62 at the time of writing — and that
directory reaches the compiler as `-isystem` ahead of `ffmpeg@6`'s own
`-isystem`. Every translation unit then compiles against FFmpeg 8 headers while
linking and running against FFmpeg 6 libraries.

Nothing complains, because the API surface Telegram Desktop uses exists in both.
The struct layouts do not match, though:

```
offsetof(AVCodecParameters, coded_side_data)   32 in ffmpeg 8,  176 in ffmpeg 6
sizeof(AVCodecParameters)                     184 in ffmpeg 8,  192 in ffmpeg 6
```

So the app reads pointers out of the wrong fields. The symptoms are segfaults
and `malloc: pointer being freed was not allocated` aborts inside libavcodec,
from any code path that touches media — playing a GIF, opening a video, even
the notification sound.

The fix is a `-I` pointing at a directory holding symlinks to `ffmpeg@6`'s
header directories, which `build_app.sh` creates at
`../tdesktop-libs/local/ffmpeg6-include`. Two things make that specific shape
necessary:

- `-I` rather than reordering, because clang searches the entire angled list
  before any `-isystem` directory, whatever the command-line order.
- A directory of its own, because clang resolves search paths to real
  directories and drops duplicates. Pointing `-I` at
  `/opt/homebrew/opt/ffmpeg@6/include` is silently a no-op: it is a symlink to
  the Cellar path that is already present as `-isystem`. This is the same trap
  as the Qt framework directory above.

To check which headers a build actually resolved, compile a static assert with
the same flags:

```c
#include <libavcodec/avcodec.h>
#include <stddef.h>
_Static_assert(offsetof(AVCodecParameters, coded_side_data) == 176, "wrong headers");
```

As with Qt, none of this applies on a machine without the full formula.

### Install

```bash
purple/install.sh
```

This copies the Qt frameworks into the bundle, re-signs it ad-hoc, and replaces
`/Applications/Purple Telegram.app`. Ad-hoc signing is enough for a locally
built app; it is not enough to distribute one. Set `Target=<path>` to deploy
somewhere else — a scratch bundle to launch and check before it replaces the
installed app.

`macdeployqt` comes from whichever Qt prefix `build_app.sh` used, by the same
rule: `../tdesktop-libs/qt-patched` when that exists, the merged Homebrew
prefix otherwise, and `QtPrefix` overrides both. The two scripts have to agree,
because a `macdeployqt` from a different Qt bundles plugins that will not load
into this one.

The first install runs `macdeployqt`. Later ones do not, because they do not
need to: the frameworks and plugins in the bundle are already there and already
relinked, and the only thing a rebuild changes is the main binary. All
`macdeployqt` still has to do is repoint that binary's 121 non-system load
commands from Homebrew and the Qt prefix into the bundle — and it does that with
one `install_name_tool` run per dependency, each rewriting the whole binary.

`purple/relink_bundle.py` does the same 121 changes in a single pass. It was
written by diffing the load commands of the pre-deploy binary against
`macdeployqt`'s output and deriving the rule, then checking that the rule
reproduces all 121 rewrites and both `LC_RPATH` edits byte for byte. The
`@executable_path/../Frameworks` rpath is part of that: the bundled Qt
frameworks resolve some of their own dependencies through `@rpath`, so without
it they would load from the build prefix instead of the bundle.

It also strips every absolute rpath from the executable, leaving only the
bundle's own - see "The rpath trap a relocatable Qt brings" for why that is
load-bearing rather than tidiness, and for the check that keeps it safe.

It refuses the fast path and hands back to `macdeployqt` whenever the bundle is
not something it can finish — never deployed, missing plugins or `qt.conf`, or
linking a library the bundle does not carry, which is what a newly added
dependency looks like. `ForceDeploy=1 purple/install.sh` skips it entirely.

The three measurements, same machine, same bundle:

    625s   unstripped binary, macdeployqt
    427s   stripped binary, macdeployqt
      4s   stripped binary, single-pass relink

Verify a fast-path install with `vmmap` on the running app: `QtCore` must
resolve inside `Contents/Frameworks`, not in the Qt prefix.

### The signing certificate, and Full Disk Access

Work Mode's focus sync reads `~/Library/DoNotDisturb/DB/Assertions.json`, which
macOS keeps behind Full Disk Access. The file mode is an ordinary
`-rw-r--r--` and the read still fails with `Operation not permitted`. Grant it
under System Settings, Privacy & Security, Full Disk Access, then relaunch -
macOS does not hand a new permission to a process that is already running.

Grant it once. Making that true took a change.

TCC keys its grants to the app's designated requirement, and an ad-hoc
signature has no certificate, so the requirement is the code hash of one exact
binary. Every build produced a different one, so every install silently revoked
the permission and focus sync went quiet until somebody read the log. Signed
with a certificate the requirement names the certificate instead:

    designated => identifier "com.tdesktop.PurpleTelegram"
        and certificate leaf = H"950fd0cb3fe60586d00eeb4d99343f33055ea0b9"

That hash is the certificate's own and does not move when the code does.
Verified rather than reasoned about: grant, rebuild, reinstall, relaunch, and
the refusal that used to appear every time is gone.

Create the certificate once:

    purple/make_signing_cert.sh

It is self-signed, needs no Apple developer account, lives in the login
keychain, and is idempotent. `install.sh` signs with it when it is present and
falls back to ad-hoc with a hint when it is not, so a fresh checkout still
installs on a machine that has never run it.

Switching an app that is already installed from ad-hoc to the certificate
changes its identity, so any permission it already held has to be granted once
more. Remove the stale entry with `-` and add the app again rather than
toggling the switch.

The script pins itself to `/usr/bin/openssl` deliberately. A Homebrew or
anaconda OpenSSL 3 earlier in `PATH` writes PKCS#12 with a SHA-256 MAC that
macOS's Security framework will not read, and `security import` then rejects it
claiming the password is wrong. The system LibreSSL writes what it expects.

Nothing else in the fork wants the permission. Everything but focus sync works
without it, and the detector says so once per launch instead of going quiet.

### Symbolicating a crash

`macdeployqt` strips the binary in place, so `install.sh` first copies the
unstripped one to `out/Purple Telegram.unstripped`. That copy has the same
`LC_UUID` as the installed binary, so a crash report matches it directly:

```bash
atos -o "out/Purple Telegram.unstripped" -l <load address> <address>
dsymutil "out/Purple Telegram.unstripped"     # standalone .dSYM, if preferred
```

Function names live in that copy and survive anything. Line numbers come from a
debug map of 1842 `OSO` entries pointing at the `.o` files under `out/`, so they
only resolve while those are intact — **symbolicate before the next build, not
after**, or run `dsymutil` once to freeze the line info into a `.dSYM`.

That debug map carries line tables and nothing else, which is all `atos` and
`dsymutil` want from it; see "How much debug info" for what is missing and how
to get it back.

`atos` sometimes prints the addresses straight back at you, unchanged, for this
binary — no name, no file. When that happens, do not conclude the symbols are
gone. Dump them and look the address up by hand:

```bash
nm -n "out/Purple Telegram.unstripped" > /tmp/syms.txt
```

`nm -n` sorts by address, so the symbol you want is the last one at or below
the address in the report, once you have subtracted the load address. That is
what actually resolved the notification crash below.

Do not be tempted to pass `-no-strip` to keep symbols in the bundle instead.
`macdeployqt` runs `install_name_tool` once per Qt framework reference and each
run rewrites the entire binary. `sample` on a slow install shows exactly that:

    deployQtFrameworks -> changeInstallName -> runInstallNameTool
        -> QProcess::waitForFinished    (child: install_name_tool)

`macdeployqt` does strip, but only after that loop finishes, so leaving the
binary unstripped for it costs the whole difference: 625 seconds measured, for a
914MB bundle. `install.sh` therefore strips before calling it — two seconds,
788MB down to 232MB, `LC_UUID` unchanged — which leaves the relink loop a third
of the work and the installed bundle at 384MB.

### What makes it "Purple Telegram"

The rebranding is deliberately four lines, so it survives rebasing onto
upstream:

- `Telegram/SourceFiles/core/version.h` — `AppName`. This is the important
  one: `platform/mac/specific_mac_p.mm` builds the data directory as
  `~/Library/Application Support/<AppName>/`, so changing it is what gives the
  app storage of its own. It also drives the tray tooltip, notification
  titles and the media-controls name.
- `Telegram/CMakeLists.txt` — `output_name`, which becomes the bundle name and
  the executable inside it, and `bundle_identifier`
  (`com.tdesktop.PurpleTelegram`), which is what Launch Services, the
  notification centre and the `tg://` handler key off.
- `Telegram/SourceFiles/core/file_utilities.cpp` —
  `DefaultDownloadPathFolder()`, which upstream derives from `AppName` and the
  fork pins to `"Telegram Desktop"` instead. This is the one place the rebrand
  is deliberately undone.

Renaming only the built bundle would not be enough. `AppName` is compiled in,
so a renamed stock build would still point at
`~/Library/Application Support/Telegram Desktop`, and the two apps would fight
over one data directory.

The data directory is the only path that should be purple. `AppName` also names
the folder downloads and exports go to, and that one belongs to you, not to the
app — a rebrand is no reason for saved files to start landing somewhere new,
next to years of them under the old name. Hence the fourth line: everything
through `File::DefaultDownloadPath()` keeps writing to
`~/Downloads/Telegram Desktop/`, shared with a stock build if you run one.

### The icon

`purple/recolour_icons.py` rotates the logo's hue from the
official blue (203°) to purple (277°), in place, across the macOS icon sets in
`Telegram/Telegram/Images.xcassets/` and the in-app artwork in
`Telegram/Resources/art/` — 37 files.

It is a pure hue rotation, so the white paper plane (saturation zero) and the
alpha channel come through untouched, and every size stays pixel-exact rather
than being resampled from one master. Re-running it on already-purple files
would rotate them a second time, so restore the originals with
`git checkout` first if you need to redo it.

The Windows `icon256.ico` is deliberately left alone, since this fork is
macOS-only for now.

### Running beside the official app

Both can run at once. `Core::Sandbox::start` in
`Telegram/SourceFiles/core/sandbox.cpp` derives its single-instance socket
from a hash of the working directory and its lock file from the executable
path, and both differ between the two apps.

### Can the two share a login?

Not as a live session, but there are two workable arrangements.

The default is simply to log in again. Purple Telegram starts empty and you
authenticate once; it is the same account, and it appears as an additional
entry under Settings → Devices. The two clients then keep entirely separate
local state and can run simultaneously.

The alternative is a one-time clone. With both apps quit:

```bash
ditto ~/Library/Application\ Support/Telegram\ Desktop/tdata \
      ~/Library/Application\ Support/Purple\ Telegram/tdata
```

The new app starts already logged in, reusing the same authorization key and
inheriting the cached history. From that point the two copies diverge, they
share a single server-side authorization — terminating it from one kills both
— and running both concurrently on one auth key is not a supported
configuration. Treat it as a migration, not as sharing.

What is not possible is pointing both apps at one data directory. The second
one to launch would find the first's local socket and hand over to it instead
of starting, so only one could ever be running.

### An isolated instance, for testing

An instance can be given its own everything, so a test run cannot reach your
account, your chats or your `settings.toml`:

```bash
XDG_CONFIG_HOME=/tmp/sandbox/config \
    "/Applications/Purple Telegram.app/Contents/MacOS/Purple Telegram" \
    -workdir /tmp/sandbox/tdata
```

`-workdir` moves `tdata`, and `XDG_CONFIG_HOME` moves `settings.toml` and
`state.toml` - see `ConfigDirectory()` in `purple_config.cpp`. The instance
starts logged out with a freshly written starter file, and the single-instance
socket is derived from the working directory, so it runs beside your own copy.

Log it in as a second session of a throwaway account rather than cloning
`tdata`, for the reason above: one authorization is one session. The account
the emulator harness uses is the obvious one, and its number is in
`~/.purple-android-test/test-account.env` - mode 600, outside every git tree,
because both forks push to public remotes. The login code arrives in the
phone's session and can be read without opening a chat:

```bash
adb shell dumpsys notification --noredact | grep -i 'login\|code'
```

**Run the deployed bundle, not `out/`.** The freshly linked binary in `out/`
resolves `@rpath` against the merged Qt prefix while the plugins macdeployqt
left in its bundle point at `Contents/Frameworks`, so launching it directly
loads two sets of Qt binaries and dies before the first window ("You might be
loading two sets of Qt binaries into the same process"). `purple/install.sh` is
what makes a runnable bundle. It is only ever launched through Finder or
`open`, which is why nothing noticed.

**Launch it from a process that is not sandboxed, and drive it with the
screen clear.** Two things cost the first attempt at driving an instance
(2026-09-11) the whole session. A binary started from an agent's sandboxed
shell inherits the sandbox: the instance came up, drew, and logged `Purple
Error: Focus state unreadable ... Operation not permitted` for the Do Not
Disturb database the deployed bundle can otherwise read, so start it through
something outside the sandbox - Hammerspoon's `hs.task.new(path, nil, args)`
with `setEnvironment` carrying `XDG_CONFIG_HOME` and `HOME`, or `open -n`.
And while a keychain authorization dialog (`SecurityAgent`, "wants to use the
System keychain") was waiting on screen from an unrelated command, nothing
synthesized reached anything: `hs.eventtap` clicks and keystrokes, an
`AXPress` on the intro's `Start Messaging` button, even an `AXPress` on the
dialog's own Deny all did nothing, while the app sat idle in its event loop
and the accessibility tree still listed the button. Only a hand at the
keyboard clears that dialog. Check for one with `pgrep -x SecurityAgent`
before starting, and treat a click that changes nothing as "look for a
dialog", not as a bug in the window. The window's own controls are reachable
through accessibility (`hs.axuielement`; the button reports its frame), which
is the way to find where to click without reading pixels.

### Headless, and how far it gets

Not far enough yet, and it is worth knowing exactly how far. The Qt **offscreen
platform plugin is not deployed** - `macdeployqt` copies only the plugin the
app asks for - but it exists in the Qt prefix, either of them. Copied into the
bundle and relinked against the bundled Qt it loads (`$QtPrefix` below is
whichever one `build_app.sh` printed):

```bash
cp "$QtPrefix/share/qt/plugins/platforms/libqoffscreen.dylib" \
   "$App/Contents/PlugIns/platforms/"
for f in QtGui QtCore; do
    install_name_tool -change "@rpath/$f.framework/Versions/A/$f" \
        "@executable_path/../Frameworks/$f.framework/Versions/A/$f" \
        "$App/Contents/PlugIns/platforms/libqoffscreen.dylib"
done
codesign --force --sign - "$App/Contents/PlugIns/platforms/libqoffscreen.dylib"
```

The relinking is the part that matters: the prefix's copy carries an rpath of
its own pointing back at the prefix, so without it the plugin drags in a second
Qt and the process dies the same way `out/` does.

With `QT_QPA_PLATFORM=offscreen` the app then gets all the way through the
config load - it writes `settings.toml`, `state.toml` and `readme.md`, runs the
schedule tick and logs it - and dies at the first window with `QRhiWidget: QRhi
is not supported on this platform`. The RHI probe itself succeeds (it finds
Metal); it is the widget that has no offscreen path. The lever is the app's own
**Enable hardware acceleration**, which lives in `tdata` and has no
command-line switch, so it would need one visible run to turn off before the
rest could be headless. Until somebody tries that, driving the desktop means a
real window on a real screen.

### Rebuilding after a change

```bash
cmake --build out --parallel
```

Ninja rebuilds only what the change touched, which for a few files is seconds
to a couple of minutes. Re-run `install.sh` to push the result to
`/Applications`.

Editing `CMakeLists.txt` costs more, but less than it first appears. Adding
source files triggers a reconfigure that rebuilds `lib_fido2` in full — around
sixty objects it could not possibly have affected — plus the automatic MOC pass,
and then only the files you actually added. That is a couple of minutes, not a
rebuild.

Changing a compiler flag is the expensive case. `CMAKE_CXX_FLAGS` and its
siblings are part of every object's compile command, so touching them correctly
invalidates all ~1500 of them. That is a full rebuild, and on this machine it
needs `--parallel 4`: ninja's default of ten clang processes exhausts memory,
and macOS responds by suspending all of them, leaving a build that appears to
hang forever with every child in state `T`.

Either way, batch the edits rather than discovering a second one halfway through.

Never run two builds against `out/` at once. `build_app.sh` both configures and
builds, so starting it while a `cmake --build` is running gives you two ninja
processes fighting over the same objects; the load average goes to three digits
and neither finishes.

### Rebasing onto upstream

The local diff is the four rebranding lines, the recoloured icon binaries,
the scripts under `purple/`, and the fork features under
`Telegram/SourceFiles/purple/` — `git grep Purple::` finds every call site where
those hook into upstream code. The icons are the only awkward part: an upstream
change to the artwork lands as a binary conflict. Resolve it by taking upstream's
files and re-running `recolour_icons.py`.

### Qt with upstream's patches

This build no longer takes Qt from Homebrew. `purple/build_qt.sh` builds
Qt 6.11.1 from source with upstream Telegram Desktop's patch set and installs
it into `../tdesktop-libs/qt-patched`, and both `build_app.sh` and
`install.sh` prefer that prefix when it exists. Homebrew's `qtbase` is the
same version with none of the patches, which is what this build used before
and what it falls back to when the prefix is not there.

Most of the set is Windows and Linux. The ones that matter here are the macOS
rendering fixes, because the app composites its whole main window through
Metal (`Renderer: [QRhi] (Window)` in `log.txt`, primed by `EnsureWindowRhi()`
in `lib_ui/ui/rhi/rhi_surface.cpp`). Patch numbers below are from the
`qtbase_6.11.1` set; the 6.11.2 set renumbers all of them.

- `0039-fix-backing-store-rhi-upload-dirty-rects`: stock Qt uploads the
  bounding rectangle of the dirty region to the GPU, so two small updates
  far apart re-upload the whole window. A CPU-resource report for this app
  (`/Library/Logs/DiagnosticReports/Purple Telegram_2026-09-03-*.cpu_resource.diag`)
  shows exactly that: the main thread at 81% for two minutes, not even
  frontmost, inside `QRhiMetal::enqueueSubresUpload` doing `memmove`.
- `0034-optimize-macos-rhi-metal-render`, `0035-macos-rhi-remove-blending`,
  `0036-macos-rhi-force-resize`, `0037-macos-rhi-three-frames-in-flight`,
  `0018-fix-backing-store-rhi-unneeded-copy`: John Preston's rework of the
  Metal backing-store path.
- `0041-macos-widget-updates-via-display-link`,
  `0042-backport-metal-monitor-plug-fix`: what happens when the external
  display sleeps or is unplugged, which this machine's log shows several
  times a day (`qt.qpa.drawing: Display requested for non-online display`).

The symptom on this machine was an app that got sluggish after hours of
uptime and display sleep/wake cycles, and was fine again after a restart,
while the official build beside it was fine throughout. The stack samples
show the process idle between interactions, so it is not a spin; it is the
per-frame cost of the unpatched compositor.

A separate defect killed the app outright twice in normal use: a native
notification carrying a user photo, dying on a freed `CGColorSpace` inside
`QImage::toCGImage()`. The High Sierra set's
`0026-fix-cgimage-colorspace-use-after-free` is that fix, and it is why that
set is applied here rather than only the main one. "The notification crash,
and why the patched Qt fixes it" below has the whole diagnosis.

If you want to test the theory without any of this, there is still a cheaper
lever: turn off "Use Qt RHI renderer" in Settings → Advanced → Experimental
settings (`kOptionUseQtRhi`, default on for Qt ≥ 6.7). The window then goes
back to the raster CALayer backing store, which the patches do not touch.
Restart required.

#### What build_qt.sh does

It clones four module repositories shallow at tag `v6.11.1` — `qt/qtbase`,
`qt/qtshadertools`, `qt/qtsvg`, `qt/qtimageformats` on GitHub — into
`../tdesktop-libs/qt-src`. Upstream clones the `qt5` super-repo and pulls the
same four as submodules; the modules carry the tag themselves, so cloning them
directly gets the identical trees without the super-repo's history. Note the
tag is `v6.11.1`, not `v6.11.1-lts-lgpl`; only the 6.2.x series uses that
suffix.

Then the two patch repositories, each checked out at the commit
`Telegram/build/prepare/prepare.py` pins:

- `desktop-app/patches` at `a17d54b63128c83cb53bd71044119e77b3a2da02`, whose
  `qtbase_6.11.1/` holds 42 patches.
- `desktop-app/qt6_highsierra_patches` at
  `4aae812a405f47553e001faf566de572d3eccd16`, 26 patches.

The pin is not caution, it is necessary. Both repositories have since moved to
6.11.2: `desktop-app/patches` HEAD has only a `qtbase_6.11.2/` directory with
46 renumbered patches, and the High Sierra set was rebased onto 6.11.2 as
well. Neither applies to a 6.11.1 tree, and neither is what upstream builds
6.11.1 with.

Patches are applied in prepare.py's order — the High Sierra set first, sorted,
then the version's own set, sorted, all with `git -C qtbase apply -v` — and
the script writes a `.purple-patched` stamp in the qtbase clone afterwards, so
a re-run does not try to apply them a second time. prepare.py feeds the whole
directory to one `xargs`; this applies them one at a time instead, which
changes nothing about the result but means a failure names the patch rather
than taking the batch down with it.

All 68 applied first time. Several landed at an offset — the largest was 218
lines, in `qtbase/src/gui/rhi/qrhi.cpp` — which is `git apply` finding the
context somewhere other than where the patch said, not a conflict. Nothing had
to be hand-fixed and nothing was skipped.

Finally the script deletes the bundled third-party copies Homebrew's formula
deletes (`double-conversion`, `freetype`, `harfbuzz-ng`, `libjpeg`, `libpng`,
`md4c`, `pcre2`, `sqlite`, `xcb`, `zlib`, and `qtimageformats/src/3rdparty`),
after patching rather than before so a patch touching them still applies.

#### Why the configure line is Homebrew's and not upstream's

Upstream configures Qt `-static -no-framework`, universal x86_64 + arm64,
`-debug-and-release -force-debug-info`, `-no-openssl -securetransport`, and
`-no-feature-futimens -no-feature-brotli -no-feature-cxx17_filesystem`. That
is the right shape for a self-contained notarized release and the wrong one
here: this route deploys with `macdeployqt`, which has frameworks to copy into
the bundle, and there are none in a static build. Nothing else in the bundle
is universal either.

So the design is Homebrew's shape plus upstream's patches: shared frameworks,
arm64 only, Release, `CMAKE_FIND_FRAMEWORK=FIRST`, the same
`INSTALL_ARCHDATADIR=share/qt` layout, and `FEATURE_system_*=ON` against the
same Homebrew libraries — brotli, dbus, double-conversion, freetype, glib,
harfbuzz, icu4c@78, jpeg-turbo, libb2, libpng, md4c, openssl@3, pcre2, zstd,
and jasper, libtiff, webp, libmng for the image formats. Several of those are
keg-only, so the script puts every one of them on `CMAKE_PREFIX_PATH` and its
`lib/pkgconfig` on `PKG_CONFIG_PATH` explicitly; Homebrew's own build gets
that from its superenv shims and would otherwise silently miss them.

Matching the library set matters beyond taste. `macdeployqt` and
`relink_bundle.py` bundle whatever the frameworks actually link, so a Qt built
against a different set produces a different bundle, and the relink rule
`relink_bundle.py` was derived from stops reproducing macdeployqt's output.

Three deliberate divergences from Homebrew:

- `-DFEATURE_vulkan=OFF`. Homebrew's qtbase records the Vulkan headers as
  living in `/opt/homebrew/include`, which drags that directory into the
  include path of every Qt-using target — the exact shadowing problem the
  section above describes, which `merge_qt_prefix.py` has to edit back out of
  `Qt6GuiDependencies.cmake` afterwards. Not building Vulkan is simpler than
  removing it, and Telegram Desktop renders through Metal and OpenGL here.
- No `CMAKE_STAGING_PREFIX`, and no `QtQmakeHelpers.cmake` edit. Both exist
  because Homebrew installs into a keg and symlinks it into `/opt/homebrew`;
  this installs into one real prefix, where stock `FEATURE_relocatable=ON`
  computes the right thing.
- `QT_BUILD_TESTS=OFF` and `QT_BUILD_EXAMPLES=OFF`, for the disk.

`qmake -query` in the resulting prefix reports `QT_INSTALL_PLUGINS` as
`.../qt-patched/share/qt/plugins`, which is the prefix's own directory rather
than a path some other formula owns. That means the copy of `macdeployqt` in
`qt-patched/bin` needs none of the `qt.conf` pinning `merge_qt_prefix.py` does
for the Homebrew route; it finds the right plugins by itself.

The four configure lines, as the script runs them (`nice -n 15`, Ninja,
`--parallel 6`):

```
cmake -S ../tdesktop-libs/qt-src/qtbase -B ../tdesktop-libs/qt-build/qtbase -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=../tdesktop-libs/qt-patched \
    "-DCMAKE_PREFIX_PATH=<the prefix>;/opt/homebrew;/opt/homebrew/opt/<each dep>" \
    -DCMAKE_FIND_FRAMEWORK=FIRST -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DQT_NO_APPLE_SDK_AND_XCODE_CHECK=ON \
    -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF \
    -DINSTALL_ARCHDATADIR=share/qt -DINSTALL_DATADIR=share/qt \
    -DINSTALL_EXAMPLESDIR=share/qt/examples -DINSTALL_MKSPECSDIR=share/qt/mkspecs \
    -DINSTALL_TESTSDIR=share/qt/tests -Wno-dev \
    -DFEATURE_sql_mysql=OFF -DFEATURE_sql_odbc=OFF -DFEATURE_sql_psql=OFF \
    -DFEATURE_openssl_linked=ON -DFEATURE_pkg_config=ON \
    -DFEATURE_system_doubleconversion=ON -DFEATURE_system_freetype=ON \
    -DFEATURE_system_harfbuzz=ON -DFEATURE_system_jpeg=ON \
    -DFEATURE_system_libb2=ON -DFEATURE_system_pcre2=ON \
    -DFEATURE_system_png=ON -DFEATURE_system_sqlite=ON \
    -DFEATURE_system_zlib=ON -DFEATURE_vulkan=OFF -DFEATURE_relocatable=ON \
    -DQT_ALLOW_SYMLINK_IN_PATHS=ON \
    -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3
```

`qtshadertools` and `qtsvg` take the common arguments and nothing else;
`qtimageformats` adds `-DFEATURE_system_tiff=ON -DFEATURE_system_webp=ON`.
The modules are configured with plain `cmake` against the prefix, which is
what Homebrew's module formulae do — `qt-configure-module` is a wrapper around
the same thing that insists on being run out of an installed Qt's `bin`.

The configure summary confirms the shape: system zlib, DoubleConversion,
libb2, PCRE2, FreeType, HarfBuzz, libjpeg, libpng, libmd4c and SQLite all
"yes"; OpenSSL 3.0 directly linked "yes"; Metal "yes"; Vulkan "no"; shared
libraries "yes"; macOS deployment tool "yes". `qt-patched/lib` comes to 54 MB,
the same as `/opt/homebrew/opt/qtbase/lib`, and the plugin set is identical to
the union of the four Homebrew kegs'.

#### The rpath trap a relocatable Qt brings

This one cost a launch. Homebrew's qtbase records absolute install names -
`/opt/homebrew/opt/qtbase/lib/QtCore.framework/Versions/A/QtCore` - so the app
linked against it carried absolute paths, `macdeployqt` rewrote each of them to
`@executable_path/../Frameworks/...`, and nothing in the bundle ever resolved
Qt through `@rpath` at all.

The patched Qt is built `FEATURE_relocatable=ON`, like every normal Qt, so its
install name is `@rpath/QtCore.framework/Versions/A/QtCore`. `macdeployqt` does
not rewrite `@rpath` entries - it has no need to, since it adds the bundle's
own rpath - and it deletes only the per-formula `/opt/homebrew/opt/<formula>/`
rpaths. What it left behind was:

    /opt/homebrew/lib
    @executable_path/../Frameworks

in that order. `/opt/homebrew/lib/QtCore.framework` is a symlink into the full
`qt` formula, 6.9.2, and dyld searches the rpath list in order, so the app
loaded 6.9.2 and died before the first window:

    Symbol not found: __ZN10QByteArray19fromPercentEncodingEOS_c
      Expected in: /opt/homebrew/Cellar/qt/6.9.2/lib/QtCore.framework/...

The same trap in a quieter form was already there under the Homebrew route:
several bundled libraries - abseil, libwebp, libbrotlicommon, libjxl_cms - do
use `@rpath`, and with `/opt/homebrew/lib` ahead of the bundle they were being
resolved from Homebrew rather than from the copies `macdeployqt` had just
placed in the bundle. It happened to work because the versions matched.

`relink_bundle.py` now drops **every** absolute rpath and keeps only
`@executable_path/../Frameworks`, and `install.sh` runs it after `macdeployqt`
as well as instead of it. Removing them is only safe if the bundle really does
carry everything reached through `@rpath`, so the script checks exactly that
first - across the executable, the bundled frameworks and the plugins - and
keeps the absolute rpaths, naming what is missing, if anything is not there.

Verify on a running instance:

```bash
vmmap <pid> | grep QtCore     # must be inside Contents/Frameworks
```

#### How the scripts choose a prefix

`build_app.sh` takes `QtPrefix` from the environment if it is set. Otherwise
it uses `../tdesktop-libs/qt-patched` when that directory exists and the
merged Homebrew prefix `../tdesktop-libs/local/qt` when it does not, and it
prints which one it picked as its first line. `install.sh` derives
`MacDeployQt` from the same rule, so the Qt the app is linked against and the
`macdeployqt` that bundles the plugins can never disagree — which is the
failure this whole area keeps producing.

To go back to Homebrew's Qt: rename or delete `../tdesktop-libs/qt-patched`,
or set `QtPrefix=../tdesktop-libs/local/qt` for both scripts. Either way the
CMake cache in `out/` remembers the old `Qt6_DIR`, so delete
`out/CMakeCache.txt` and let `build_app.sh` reconfigure. That is a full
rebuild of the Telegram target, because every translation unit's include path
changes.

The bundle also has to be re-deployed rather than fast-path relinked, since
the frameworks already inside it come from the other Qt. Delete
`out/Purple Telegram.app/Contents/Frameworks`, `Contents/PlugIns` and
`Contents/Resources/qt.conf`, then `ForceDeploy=1 purple/install.sh`.

`install.sh` also honours `Target`, so a build can be deployed somewhere other
than `/Applications` and launched from there before it replaces the installed
app.

#### Cost, and what it left behind

Wall clock on this machine, eight cores, `--parallel 6`, everything under
`nice -n 15` with the machine in normal use:

    39s        qtbase configure
    5m 0s      qtbase build and install
    37s        qtshadertools
    18s        qtsvg
    24s        qtimageformats
    58m 33s    the Telegram target, reconfigured and rebuilt at --parallel 4
    ~4m        macdeployqt on the new bundle, plus signing and install

Under six minutes for Qt, which is not what "build Qt from source" usually
costs. Release, one architecture, no tests, no examples and no QtDeclarative
is most of the difference; upstream's several hours are for a debug-and-release
universal static build of a much longer module list.

Disk, on a volume that started with 24 GB free:

    410 MB     ../tdesktop-libs/qt-src, the four shallow clones plus the two
               patch repositories
     80 MB     ../tdesktop-libs/qt-patched, the installed prefix
      0        ../tdesktop-libs/qt-build, deleted per module after its install

Build trees are removed as each module installs, so peak usage is one module's
tree rather than four. The sources are worth keeping: they carry the applied
patches and the stamp, so a re-run rebuilds without re-cloning or re-patching.

#### The notification crash, and why the patched Qt fixes it

The two reports (2026-09-07 20:31, 2026-09-08 22:56) are the same crash
byte for byte — identical image offsets on every frame. `EXC_BAD_ACCESS`,
`KERN_INVALID_ADDRESS`, main thread, on the notification timer:

    base::Timer::timerEvent
      Window::Notifications::System::showNext
        NativeManager::doShowNotification
          Platform::Notifications::Manager::Private::showNotification
            Platform::Q2NSImage -> QImage::toCGImage()
              CGImageCreate -> verify_image_parameters
                valid_image_colorspace -> CGColorSpaceGetType -> objc_msgSend

`x0`, the `CGColorSpaceRef`, was a well-formed MALLOC_NANO pointer whose first
word had been overwritten with a nano free-list link. The colour space had been
freed and the memory recycled.

The defect is in Qt 6.11.1's `qt_mac_cgImageFormatForImage`
(`src/gui/painting/qcoregraphics.mm`). It builds the colour space into a local
`QCFType<CGColorSpaceRef>` and returns it inside a plain `vImage_CGImageFormat`,
which does not retain — so the caller receives a pointer to an object that has
already been released once. Whether that is fatal depends entirely on who else
holds a reference, which is why it looked intermittent:

- An image with no colour space at all, the common case, takes
  `CGColorSpaceCreateWithName(kCGColorSpaceSRGB)`, an immortal process-wide
  singleton with a retain count of `UINT32_MAX`. Releasing it does nothing.
  Same for ICC data CoreGraphics recognises as a system profile.
- An unrecognised embedded profile — a camera or phone profile in a real
  photo — produces an ordinary object, kept alive only by a bounded internal
  CoreGraphics cache. When that entry is evicted the pointer dangles, and the
  next notification carrying that userpic dies.

Qt's JPEG and PNG readers attach embedded profiles by default
(`qjpeghandler.cpp` calls `setColorSpace(QColorSpace::fromIccProfile(...))`,
`qpnghandler.cpp` does the same on `iCCP`/`sRGB`/`gAMA` chunks), `QImage::scaled`
and `convertToFormat` keep them, and nothing in this tree ever calls
`setColorSpace` or `convertToColorSpace`. So the fatal input is a peer's cloud
userpic photo with an unusual profile, reaching `Q2NSImage` through
`GenerateUserpic` and `PeerData::GenerateUserpicImage` (`data_peer.cpp`) in the
legacy `NSUserNotification` manager (`platform/mac/notifications_manager_mac.mm`).
Letter avatars and Saved Messages are painted into a fresh image and are safe.
The `UNUserNotificationCenter` manager writes a PNG and never calls `Q2NSImage`,
but it sits behind the experimental `kOptionMacModernNotifications`, off by
default — so every stock build takes the crashing route, and the only thing
that differs from upstream's own build is the Qt underneath.

Upstream Qt fixed it in qtbase `08e464f8559719662c5b2207d1d4c84c251eca07`,
"Fix regression in QImage::toCGImage() with custom color space"
(QTBUG-147602, June 2026), which landed in 6.11.2 — after the version this
build pins. The High Sierra set's `0026-fix-cgimage-colorspace-use-after-free`,
which this build applies, reaches the same end for the three call sites that
exist (`QImage::toCGImage`, `QMacCGContext`, `qfontengine_coretext.mm`) by
returning +1 and adopting at each caller, and additionally falls back to named
sRGB when the ICC data is rejected.

Note that upstream's `0013-convert-qimage-to-srgb` and `0014-lcms2` do not
avoid this on their own: they leave decoded images with a still-valid sRGB
`QColorSpace`, so the ICC path stays in use. It is 0026 that makes it safe.

There is deliberately no guard in `Q2NSImage`. The defect is gone at its source
in the Qt this build now ships; a guard would cover one of the three doors;
`lib_base` is a submodule with only upstream's remote, so a change there is
either an uncommittable dirty submodule or a fork remote to carry across every
bump; and `setColorSpace(QColorSpace())` would render a Display P3 userpic
slightly desaturated.

That leaves exactly one residual risk: a rebuild that quietly falls back to
Homebrew's Qt brings the crash straight back. `install.sh` therefore compares
the bundle's `QtGui` against `/opt/homebrew/opt/qtbase`'s after deploying, and
prints a warning naming this crash if they are byte-identical. It does not
fail — the Homebrew route is still supported, it simply has this defect.

#### Rolling back an install

`install.sh` replaces `/Applications/Purple Telegram.app` outright. Before
switching to a Qt this app has never run on, move the old bundle aside rather
than overwriting it:

```bash
mv "/Applications/Purple Telegram.app" "/Applications/Purple Telegram.app.prev"
ditto "<the new bundle>" "/Applications/Purple Telegram.app"
```

Undoing that is the same two commands the other way round. The previous
build's symbols survive alongside it as `out/Purple Telegram.unstripped.prev`,
copied before `install.sh` overwrote `out/Purple Telegram.unstripped`, so a
crash report from the old binary still symbolicates — see "Symbolicating a
crash", and remember the line numbers were already gone the moment `out/` was
rebuilt.

#### The check that was actually run

Building is not evidence. The bundle was deployed to a scratch `Target` and
launched as an isolated instance (see "An isolated instance, for testing"),
with its own `XDG_CONFIG_HOME` and `-workdir`, so it could not reach a real
account. What the log has to show is `Renderer: [QRhi] (Window)` — that is the
Metal path the patches are for; anything else means the app fell back and the
patches are not being exercised — followed by the app reaching the intro
screen with no new report in `~/Library/Logs/DiagnosticReports/`.

It earned its keep the first time it ran: the app died at launch on the wrong
QtCore, which is the rpath problem above, and nothing short of starting the
binary would have shown it. The second run reached the intro screen, logged

    RHI: Probe backend=Metal device=Apple M2 compute=yes.
    QRhi: backing store primed for window
    Renderer: [QRhi] (Window)
    QRhi: SurfaceRhi created

stayed up, and left no crash report. `vmmap` on it confirmed `QtCore` loaded
from `Contents/Frameworks`.

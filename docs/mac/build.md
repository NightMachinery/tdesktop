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
`Telegram/build/qt_version.py` pins, so Qt is a download rather than a build.
That brings the whole thing down to roughly 12 GB and a few hours, nearly all
of it the Telegram target itself.

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
optimized enough to live in the Dock, but with symbols so a debugger can
still attach to code you are changing.

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

This is worth understanding before changing anything in the scripts, because
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
is harmless there.

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
built app; it is not enough to distribute one.

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

### Full Disk Access, and why every install loses it

Work Mode's focus sync reads `~/Library/DoNotDisturb/DB/Assertions.json`, which
macOS keeps behind Full Disk Access. The file mode is an ordinary
`-rw-r--r--`, and the read still fails with `Operation not permitted`. Grant it
under System Settings, Privacy & Security, Full Disk Access, then relaunch -
macOS does not hand a new permission to a process that is already running.

The grant does not survive `install.sh`. TCC keys it to the app's designated
requirement, and for an ad-hoc signature that is the code hash of one exact
binary, which every build changes. Measured rather than assumed: the grant was
made, the app rebuilt and reinstalled, and the next launch logged

    Purple Error: Focus state unreadable, cannot open it (Operation not
    permitted) - Full Disk Access for Purple Telegram is what this usually
    wants.

So re-approving is part of installing, for as long as the signature is ad-hoc.
Signing with a stable self-signed certificate would fix it properly: the
designated requirement would then name the identifier and the certificate
rather than a hash, and would hold across rebuilds. That means a keychain
certificate to create and a change to `install.sh`, and it has not been done.

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

The rebranding is deliberately three lines, so it survives rebasing onto
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

Renaming only the built bundle would not be enough. `AppName` is compiled in,
so a renamed stock build would still point at
`~/Library/Application Support/Telegram Desktop`, and the two apps would fight
over one data directory.

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

The local diff is the three rebranding lines, the recoloured icon binaries,
the scripts under `purple/`, and the fork features under
`Telegram/SourceFiles/purple/` — `git grep Purple::` finds every call site where
those hook into upstream code. The icons are the only awkward part: an upstream
change to the artwork lands as a binary conflict. Resolve it by taking upstream's
files and re-running `recolour_icons.py`.

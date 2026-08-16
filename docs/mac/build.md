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
file, because this checkout's `origin` is upstream `telegramdesktop/tdesktop`:

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

`merge_qt_prefix.py` additionally drops `WrapVulkanHeaders` from Qt6::Gui's
interface. Homebrew's qtbase records the Vulkan headers as living in
`/opt/homebrew/include`, which put that directory in the include path of
every Qt-using target. It is not the only source of that directory, so it is
not sufficient on its own, but Telegram Desktop renders through Metal and
OpenGL on macOS and never needs it.

None of this applies on a machine without the `qt` formula; the merged prefix
is harmless there.

### Install

```bash
purple/install.sh
```

This runs `macdeployqt` to copy the Qt frameworks into the bundle, re-signs it
ad-hoc, and replaces `/Applications/Purple Telegram.app`. Ad-hoc signing is
enough for a locally built app; it is not enough to distribute one.

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

### Rebasing onto upstream

The local diff is the three rebranding lines, the recoloured icon binaries,
and the scripts under `purple/`. The icons are the only
awkward part: an upstream change to the artwork lands as a binary conflict.
Resolve it by taking upstream's files and re-running `recolour_icons.py`.

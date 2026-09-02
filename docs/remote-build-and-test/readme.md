# Building and testing the Android fork on a remote machine

The Android side of Purple Telegram, `NightMachinery/purple-telegram-android`,
is built on a shared institutional Linux server referred to here as the build
box, and tested there in a headless emulator. The Mac only holds the release
keystore and signs the artifacts it hands out.

Everything here assumes the layout that repository's README describes: the
checkout, the Android SDK, the Qt for Android prefix and a gitignored
`local.properties` carrying the api credentials.

## Why a remote box at all

An Android build wants 20 GB and a lot of cores. The Mac has neither to spare.
The build box has 96 cores and a terabyte of memory, so a clean build takes
about an hour there even while other people are using it, and an incremental
one a few minutes.

The cost is that the machine is shared. Other users and administrators can read
anything world-readable, so a test account signed in inside an emulator there
would otherwise be visible to them.

## Keeping the session private

Two mechanisms, both without root.

Everything under the working directory and the SDK is mode 700. That covers the
checkout, the gradle cache, the AVD and its disk images, and the throwaway
keystore.

The emulator and the adb server run inside a private network namespace, so
their listening ports do not exist for anyone else. `bin/emu-ns.sh` in the
working directory creates it:

```
emu-ns.sh up            # create the namespace and its outbound link
emu-ns.sh run CMD...    # run CMD inside it
emu-ns.sh down          # tear it down, killing everything inside
```

It uses an unprivileged user namespace to hold the network namespace, and
`slirp4netns` (a static binary, downloaded once) to give it outbound
connectivity through a userspace TCP/IP stack. `--disable-host-loopback` stops
anything inside from reaching the host's own loopback services.

Two details are easy to get wrong. The namespace needs its own
`/etc/resolv.conf`, bind-mounted from the working directory, because the host's
resolvers are unreachable from inside; the addresses to use are the ones
slirp4netns provides. And when the host's resolvers are IPv6-only, the IPv6
address and default route have to be added by hand, because `--configure` only
sets up IPv4.

The result is that `ss -ltn` on the host shows nothing on 5037, 5554 or 5555
while the emulator is running, and only processes started through the wrapper
can talk to it.

The adb server itself must stay on TCP 5037 inside the namespace. Moving it to
a unix socket with `ADB_SERVER_SOCKET` looks tidier but breaks the emulator,
which registers itself with a server on the TCP port and will not find one
anywhere else.

## What must never go on the build box

The release keystore, any Telegram session belonging to the main account, a git
identity, or a GitHub key. The build box only ever pulls public repositories,
and commits are authored and pushed from the Mac.

The one credential that lives there is the api id and hash pair in
`local.properties`, mode 600 and gitignored. Both are extractable from any
Telegram APK, so their exposure class is low.

Test builds are signed with a throwaway key generated on the box, whose
password is written in the clear in the scripts. The emulator refuses to
install an unsigned APK, and that is the only thing this key is for. Anything a
person installs is signed on the Mac with the real key instead.

## Building an uncommitted patch

Commits are authored on the Mac and the build box only pulls, so a change
that is not pushed yet cannot be built through `bin/rebuild.sh`. For that
there is `bin/build-patch.sh`: it resets the checkout to `origin/master`,
applies a diff, builds, and signs the result with the throwaway key exactly
as `rebuild.sh` does.

The diff is sent first and the build started detached, because a build takes
minutes and the patch must not be read from the stdin of a backgrounded
process:

```
git add -N path/to/NewFile.java      # intent-to-add, so the diff includes new files
git diff HEAD --binary | ssh pi 'cat > /tmp/purple-android/pending.patch'
ssh pi 'nohup /tmp/purple-android/bin/build-patch.sh > /tmp/purple-android/fork-b/build.log 2>&1 &'
```

Poll the log for `BUILD_EXIT=`; a compile error shows up as `error:` lines
above it. Because the checkout is reset every time, nothing from a previous
patch survives, and a patch that applies on the Mac applies there too.

This is the loop a delegated implementation agent uses to verify its work
before anything is committed. The one thing it must never do is copy
anything but the patch to the box.

## Running the emulator

An x86_64 system image with Google APIs runs the arm64-only APK through
Android's ARM translation, so no separate arm64 image is needed. `/dev/kvm`
must be readable; on the build box it is world-accessible.

`bin/emu-start.sh` boots the AVD inside the namespace and waits for it. It
kills any stale emulator process first, because a crashed one leaves a
half-dead virtual machine holding the port. Boot takes about 30 seconds.

Note the hazard that made that necessary: `pkill -f` matches the whole command
line, including the command you are running. Calling `pkill -f qemu…` inline
over ssh kills the ssh session itself. Inside a script file the pattern is not
in the script's own command line, so it is safe there.

## The AVD settings that matter

A freshly created AVD came out with GPU emulation disabled, which sends the
emulator down a rendering path that segfaults within seconds of the app drawing
a chat. Set these in the AVD's `config.ini`:

```ini
hw.gpu.enabled=yes
hw.gpu.mode=swiftshader_indirect
hw.ramSize=8192
hw.cpu.ncore=8
vm.heapSize=512M
```

The memory and core counts are comfort rather than necessity. The GPU lines are
the fix.

## The emulator still segfaults sometimes

Even configured correctly, the emulator's software renderer dies with a
segmentation fault under heavy drawing. The whole emulator process exits with
status 139 and adb reports `error: closed`. It is a host-side renderer crash,
not a crash of the app, and it takes the app down with it.

Three things reduce it, and none eliminates it:

- Put the app in full power-saver mode, which turns off chat blur and every
  animation. Write `<int name="lite_mode" value="0" />` into the app's
  `mainconfig.xml` with the app stopped. This is the single biggest
  improvement.
- Use the canary emulator (`sdkmanager --channel=3 emulator`). It survives
  screens the stable build did not.
- Prefer the emulator's own capture, `adb emu screenrecord screenshot <dir>`,
  over `adb shell screencap`. Both read back through the renderer and both can
  crash it, but the console path proved more reliable.

The share sheet's send path is the most reliable way to trigger it. Sending a
file from the attach menu inside a chat instead avoids it entirely.

Design around the crashes rather than fighting them. The AVD's disk image
persists, so a crash costs a restart and nothing else. Do one step per boot
when a step is fragile, and check `grep EXIT= emulator.log` after each.

Attaching gdb is not worth it. QEMU uses SIGUSR1 constantly, and a debugger
that stops on signals slows the boot past any useful timeout.

## Verifying without the screen

Telegram draws its chat list and message cells as custom views with no text
nodes, so `uiautomator dump` shows almost nothing for them. Ordinary dialogs
are real views and do appear, which makes the dump a good way to read a
confirmation dialog's text when a screenshot would risk the renderer.

The app's own log is more useful than logcat for anything network-related.
Enable it by writing `<boolean name="logsEnabled" value="true" />` into
`shared_prefs/systemConfig.xml`, then read
`/sdcard/Android/data/<applicationId>/files/logs/*_net.txt`.

## Logging in a test account

Use a real account on the production servers. Telegram's test data centres and
their documented fixed login codes were rejected with `PHONE_CODE_INVALID` on
every data centre when this was tried, and the app's own network log confirmed
the rejection came from the server.

The login code arrives in the account's other Telegram session rather than by
SMS, so someone has to read it out. Deny every runtime permission the app asks
for; none of them matter for this.

For a settings.toml round trip, push the file to the emulator's `Download`
folder, open Saved Messages through the chat search (an empty Saved Messages is
not in the chat list), then attach it from inside the chat with the paperclip,
choosing File and then Internal Storage.

## Keeping the login between test runs

Logging in needs a code read out of another device, so do it once and keep the
result. `bin/session-save.sh` snapshots the app's whole data directory into
`session-org.purple.telegram.tar.gz`, and `bin/session-restore.sh` puts it back
after a reinstall or a `pm clear`. Both stop the app first, and the restore
fixes up ownership afterwards, because the package's uid changes on every
reinstall.

Take the snapshot once, right after logging in. From then on, a test run is
install, restore, go.

Reinstalling over the top with `adb install -r` already keeps the data, so the
snapshot is for the cases that do not: a wiped AVD, a deliberate `pm clear`, or
a rebuilt emulator. Take a fresh snapshot whenever the account's state changes
in a way worth keeping.

The snapshot contains a live Telegram authorization key, so treat it like one.
It is written mode 600 inside the mode-700 working directory, which is
deliberately outside every checkout, and it must only ever hold a throwaway
test account. The Android fork also ignores `session-*.tar.gz`, so a snapshot
that ends up inside the repository by accident cannot be committed. If one
leaks, or when testing is over for good, terminate that session from Telegram's
Settings, Devices on the real phone: deleting the file does not revoke the key.

The archive is built inside the guest and then pulled, rather than streamed out
of `adb exec-out`. The emulator can segfault mid-stream, and streaming left a
zero-byte archive that looked like a success. The script now refuses to keep
anything implausibly small and checks the pulled size against the original.

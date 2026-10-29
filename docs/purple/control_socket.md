# Local automation and tdesktop-cli

Purple exposes an optional local control socket for desktop hotkeys and scripts.
Open Settings, Advanced, Purple, Local automation and confirm both dialogs to
allow commands. Automation starts off. A socket request cannot open a consent
dialog, enable automation or change settings.

This version supports `hello`, `ping`, `window.list`, `window.activate` and
`window.cycle`. Window commands use the old primary-first window order and
refuse while the app passcode is locked. All five capability groups start on:
window, media.control, media.info, lock and emoji. Only window commands are
implemented here. Media, lock, artwork rendering and subscriptions are later
slices. `hello` advertises the implemented commands, build and enabled groups.
All commands except `hello`, including `ping`, require the master switch.

## Try the CLI

The standard-library-only Go module is under `purple/cli/tdesktop-cli`. With a
local Go 1.26 or later toolchain:

```sh
cd purple/cli/tdesktop-cli
go test ./...
go build -o tdesktop-cli .
./tdesktop-cli hello --json
./tdesktop-cli windows
./tdesktop-cli activate 0
./tdesktop-cli cycle
```

The CLI accepts `--socket PATH` and `--json` before or after the command. Socket
discovery uses that flag, then `PURPLE_CONTROL_SOCKET`, then the app's default
profile. On macOS that is `~/Library/Application Support/Purple Telegram`.
On Linux and other supported Unix systems, an existing
`~/.TelegramDesktop/tdata/settings0`, `settings1` or `settingss` selects the
legacy `~/.TelegramDesktop` profile first. Otherwise it uses
`$XDG_DATA_HOME/TelegramDesktop`, with `~/.local/share/TelegramDesktop` when
XDG_DATA_HOME is empty or relative. A default socket path of 104 or more
UTF-8 bytes instead uses `~/Library/Application Support/Purple/control.sock`,
matching the app on every supported platform. The CLI never scans
single-instance sockets. A call has a two-second connection deadline and a
two-second exchange deadline. Human window titles are quoted to keep line breaks
and terminal controls from changing the output layout. JSON output is the full
v1 response envelope.

Exit codes are 0 for success, 2 for automation/capability/lock refusal, 3 for no
reachable socket, 4 for an unsupported protocol or command, and 1 for other
errors. The CLI prints local enable instructions when automation is disabled.
The CMake app target does not build or install this separate command.

## Paths and ownership

The app opens its default `<working directory>/purple-control.sock` only on its
default working directory. A path of 104 or more UTF-8 bytes uses the shorter
fallback above. Alternate and test profiles open no socket until a path is
configured explicitly. The optional catalog v1 field `controlSocket` publishes
the current listening path; it is absent when no listener is available. Catalog
updates follow listener readiness and path changes through a cached main-thread
value. They do not add filesystem work on the GUI thread.

The native Unix listener uses a 0600 socket in an owned 0700 directory. Every
accepted peer must have the effective user's uid (`getpeereid` on macOS/BSD,
`SO_PEERCRED` where supported). Unknown peer-credential platforms and Windows
fail closed. Configured paths must be absolute, clean, non-link paths shorter
than 104 UTF-8 bytes. Missing dedicated parents are created 0700. Existing
non-private parents are refused, never chmodded. Choose a dedicated 0700 folder
if the default profile's existing directory is not private. Invalid stored
UTF-8 refuses listening until the local setting is corrected.

An existing endpoint is replaced only when it is the same owned socket after a
nonblocking probe reports no listener. Live sockets, files, symlinks and foreign
owners are preserved. Shutdown checks the socket and parent identities before
unlinking; replacing the public pathname leaves that replacement intact.
Processes running as the same user are trusted and can already read or modify
these files directly. Identity checks do not claim confinement against that
user racing the final filesystem call.

Qt's Unix QLocalServer closes by unlinking its recorded listener name, and its
base pending queue expects a Qt-created listener notifier. Purple therefore owns
the native listener and notifier on a worker. Its QLocalServer subclass owns
QLocalSocket children and provides a one-entry pending bridge through the public
virtual `hasPendingConnections` and `nextPendingConnection` methods and
`newConnection` signal. It does not call Qt listen, its base pending queue or
private Qt APIs. Only native endpoint ownership removes the public socket.

## Protocol and resource bounds

One UTF-8 JSON object per line, with a 65,536-byte limit excluding the LF:

```json
{"v":1,"id":1,"cmd":"window.activate","args":{"index":0}}
{"v":1,"id":1,"ok":true,"result":{"activated":0}}
```

An error has `ok:false` and `error:{code,message}`. Request IDs are exact signed
64-bit integers. Required integers (the version, request ID and window index)
are checked against their original JSON number tokens. Decimal and exponent
forms are accepted only when their exact mathematical value is an integer
within the signed 64-bit range. Overflow and fractions are rejected before
rounding can change request identity or invoke an action. JSON strings do not
stand in for numbers. A parallel token document preserves escaped member names
and the JSON parser's member selection; argument tokens travel with the request
to the main-thread handler. The token document is at most twice the bounded
64 KiB input frame.
Invalid UTF-8 and malformed envelopes never invoke a command. Split frames and several requests on one connection work;
in-flight duplicate IDs return busy. Oversized unterminated lines disconnect.
Responses are capped at the same line limit. Window snapshots are capped at 64
windows and 128 UTF-16 units per title. An oversized result becomes busy.

There are at most 16 active clients, eight pending requests per client and 64
pending overall. A separate 64-slot lease bounds queued main-thread dispatches,
including cancelled events awaiting consumption. Rapid disconnects therefore
cannot grow the GUI event queue without bound. Each connection allows a burst
of 20 requests and replenishes ten per monotonic second. Exceeding that rate
closes the connection. Outbound buffering is capped at 128 KiB per connection;
a slow consumer exceeding it is disconnected. A pending command expires
after ten seconds; idle clients expire after 30 seconds.

Socket and filesystem operations run on a dedicated QThread. Main-thread
handlers use copied requests and immutable policy snapshots. Disconnect, expiry,
reconfiguration and shutdown cancel tickets; main admission checks the ticket,
server epoch and policy generation before acting. Replies repeat those checks
and re-authorize after policy changes. Async handlers must reply on the main
owner thread, as the existing exporter completion callbacks do; replies on a
wrong thread are discarded. Late replies cannot address a replacement client
or revive a destroyed owner. The worker never waits on main. Shutdown stops and
joins the worker before the main owner disappears. Atomic snapshot/lease access
holds no lock while invoking a handler, status callback or filesystem operation.

Local preferences use the existing `automation.enabled` key, `automation.*`
group keys and `automation.socket-path`; nothing is added to sequential binary
settings serialization. Byte-array preference definitions live in Purple's own
service unit and preserve the generic KV bytes. Two-step consent tokens retire
on local disable or reconfiguration, so an old dialog cannot enable a new intent.

## Validation and upstream hooks

`purple/test_control.py` compiles all changed production units using the Debug
build's recipes, physical stdafx and language headers generated from this tree.
It runs actual worker socket/protocol/authorization/window-adapter tests, literal
preference bodies plus a production-symbol link check, static CTRL removal and
Go fake-server tests. Every test socket is in its own private temporary directory;
no GUI, account, installed client or Telegram request is needed. The canonical
`test_control` gate registers both native programs and the Go tests.

`core/sandbox.cpp` loses only the fork's CTRL include and branch. The upstream
CMD, OPEN and XDG activation branches stay intact. The fork-only
`core/external_control.cpp` and `.h`, and their old Telegram/CMakeLists entries,
are removed. All new implementation and registration live in Purple files and
Telegram/cmake/purple.cmake. See [upstream_hooks.md](upstream_hooks.md).

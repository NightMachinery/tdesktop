# Local automation and tdesktop-cli

Purple exposes an optional local control socket for desktop hotkeys and scripts.
Open Settings, Advanced, Purple, Local automation and confirm both dialogs to
allow commands. Automation starts off. A socket request cannot open a consent
dialog, enable automation or change settings.

This version supports `hello`, `ping`, `window.list`, `window.activate`,
`window.cycle`, `emoji.render`, `media.toggle`, `media.play`, `media.pause`,
`media.now`, `lock`, `subscribe` and `unsubscribe`. Window commands keep the
primary-first order. Lock refusal is configurable per capability as described
below. All five capability groups start
on: window, media.control, media.info, lock and emoji. `hello` advertises the
implemented commands, build and enabled groups. All commands except `hello`,
including `ping`, require the master switch.

## Allow while locked

In Settings, Advanced, Purple, Local automation, each capability has a local
"Allow while locked" switch. Media information has separate music and voice
switches. Defaults also apply to existing installations without saved switches:

- Window commands: refused while locked, since titles can contain chat names.
- Playback controls: allowed while locked.
- Music information: allowed while locked, including title, performer and a
  verified downloaded file path.
- Voice information: refused while locked, including an implicit active voice
  item selected by `media.now` or `media.state`.
- Emoji rendering and the local export session: allowed while locked.
- Lock itself: always allowed by lock policy, with no switch.

The master switch and capability groups still apply. Only the settings UI can
change these device-local preferences; no socket command can read-modify them.
The settings UI remains inaccessible while locked. Work Mode withholding applies
on top of lock policy, so enabling voice while locked does not reveal hidden
sender or chat fields. The actual player type is carried internally even when
Work Mode omits it from JSON. An empty player has no media metadata to refuse;
a successful result missing its internal type requires both information switches.

Commands and subscription events use one policy function, checked immediately
before queuing bytes to the socket. A switch disabled while locked affects the
next send. An in-flight request crossing a policy generation returns the current
authorization error, or `busy` if still authorized. Subscriptions retain their
existing epoch and generation fences and refresh after policy changes. Bytes
already queued to the operating system cannot be retracted.

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
./tdesktop-cli media toggle
./tdesktop-cli media pause --voice
./tdesktop-cli media now --json
./tdesktop-cli voice now
./tdesktop-cli lock
./tdesktop-cli emoji render 5339220291157500400 --px 256
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
two-second exchange deadline for ordinary commands. Emoji rendering has a
ten-second exchange deadline, covering the app's eight-second render wait.
Human window titles and rendered paths are quoted to keep line breaks
and terminal controls from changing the output layout. JSON output is the full
v1 response envelope.

Exit codes are 0 for success, 2 for automation/capability/lock refusal, 3 for no
reachable socket, 4 for an unsupported protocol or command, and 1 for other
errors. The CLI prints local enable instructions when automation is disabled.
The CMake app target does not build or install this separate command.

## Media and lock

`media.toggle`, `media.play` and `media.pause` belong to `media.control`.
Their only optional argument is `type:"music"` or `type:"voice"`; omission uses
the player's active type. They use the existing player and return its selected
`type` and resulting `state`. No current item returns `not-found`.

`media.now` belongs to `media.info`. It accepts the same optional type selector;
`voice now` in the CLI supplies `type:"voice"`. A normal result contains `type`,
`state` (`playing`, `paused` or `stopped`), `position_ms`, `duration_ms`, and
`file:{name,path,mime}`. Music adds `music:{title,performer}`; voice adds
`voice:{sender,chat,date}`, with date in Unix seconds. No current item has a
null type, stopped state and an empty file. A missing or not-downloaded file has
a null path. The app never downloads a file to answer this command.

The app copies cached metadata on main, verifies a candidate regular non-link
file on a worker, then recollects the current metadata and Work Mode visibility
on main before replying. A path is returned only when that fresh snapshot still
names the verified file. If Work Mode hides the current chat, the entire result
is `hidden:true` and `file:{name,path}`. Sender, chat, MIME, timestamps, playback
state and music metadata are withheld. Missing message context while filtering
is treated as hidden. Paths describe a local snapshot and may disappear later.
Media information follows the actual music or voice lock switch, including
when lock changes during an asynchronous path check. A refused result contains
only a typed `locked` error, even when its Work Mode wire form would omit type.

`lock` belongs to `lock`, takes no arguments, and returns `locked:true` after the
existing local passcode lock succeeds. It is idempotent when already locked.
Without a configured passcode it returns `no-passcode`. Its successful reply
survives the policy generation change caused by its own lock operation; master
or lock-capability revocation still fences that reply. No unlock command exists.

The CLI's `--voice` and `--music` select playback controls; `media now` reads the
active type and `voice now` explicitly reads voice. Human output quotes file
paths and metadata. JSON returns the unchanged response envelope. A hidden
result prints only its file fields and the hidden marker.

## Emoji rendering

`emoji.render` takes `args:{id,px?}` in the `emoji` capability group. It requires
Local automation, the emoji group, an active account and Local emoji export.
While locked, its allow switch must also be on (the default). The optional pixel bound defaults to 512 and accepts exact integers
from 1 through 512. Artwork IDs are nonzero uint64 values, checked against their
original JSON tokens without floating-point rounding. Exact decimal and exponent
forms work within that range. A catalog's string ID also works when it is the
canonical ASCII decimal spelling: no sign, whitespace or leading zero. Request
correlation IDs remain signed64 and are independent of artwork IDs.

A successful asynchronous reply retains the request's correlation ID:

```json
{"v":1,"id":7,"cmd":"emoji.render","args":{"id":"5339220291157500400","px":256}}
{"v":1,"id":7,"ok":true,"result":{"path":"/owned/emoji-export/large/5339220291157500400-256.png","px":256,"sourceKind":"tgs"}}
```

The command shares [the Large demand queue and renderer](emoji_export.md),
including its 64-item admission bound, spacing and cooldown. Concurrent requests
for one artwork share the demand and retain separate replies and pixel bounds.
At most 64 socket waiters are retained. A full or cooling queue returns `busy`.
Unknown artwork returns `not-found`; non-emoji, oversized or unrenderable artwork
returns `unsupported`. Disabled or unavailable export returns `export-disabled`.
The existing `automation-disabled`, `capability-off` and `locked` refusals apply
both at admission and again before sending a late reply. An unfinished request
returns `timeout` after eight monotonic seconds, checked every 100 milliseconds.

Publication and scaling run on the existing export worker. The worker reads a
verified owned PNG capped at 4 MiB and 512 pixels per side, scales down with its
aspect ratio intact and never upscales a native raster. `px` reports the actual
longer side, which may be below the requested bound. `sourceKind` is `tgs`,
`webp` or `webm`. Each variant is written through the Large store, participates
in its existing file and byte caps, and is advertised in the atomic catalog
before its nonempty regular file is checked again and the reply is completed.
The exporter retains its full-size source metadata so a later larger request
does not upscale an earlier small variant. A missing or evicted source returns
`not-found`, allowing a fresh demand.

Disconnect, server stop and pending expiry retire the request's active ticket.
Cancelled callbacks are dropped within the next 100 ms check. Shared artwork
work may finish for other consumers; it retains no dead socket. Epochs and
per-waiter serials make old worker completions inert after revocation or rearm.
Multiple outstanding requests on one connection are supported; duplicate
correlation IDs return `busy` without replacing the original request.
The request-file fallback remains available as documented in the export guide.
Paths are cache entries and may later be evicted, as with request-file results.

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
stand in for those numbers. The artwork ID exception is described below. A parallel token document preserves escaped member names
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

The media fixture uses a thin value adapter and inert synthetic files. It tests
music/voice/empty/not-downloaded snapshots, Work Mode withholding, argument and
capability refusals, file checks on their production worker thread, and a real
socket lock completion across policy changes. The production player and passcode
adapter is compiled against app headers. Real playback and a live-profile lock
remain native integration checks; host qualification does not open a profile or
change audio playback.

`purple/test_control.py` compiles all changed production units using the Debug
build's recipes, physical stdafx and language headers generated from this tree.
It runs actual worker socket/protocol/authorization/window-adapter tests, literal
preference bodies plus a production-symbol link check, static CTRL removal and
media snapshot/adapter/lock-generation tests and Go fake-server tests. Every test socket is in its own private temporary directory;
no GUI, account, installed client or Telegram request is needed. The canonical
`test_control` gate registers all four native programs and the Go tests.
The emoji program uses the actual server, bounded waiter broker, literal
exporter admission/serving methods and production disk worker with synthetic
source PNGs. It covers exact uint64/pixel parsing, concurrent replies, duplicate
IDs, cancellation, revocation, timeout, publication and no-upscale variants.

`core/sandbox.cpp` loses only the fork's CTRL include and branch. The upstream
CMD, OPEN and XDG activation branches stay intact. The fork-only
`core/external_control.cpp` and `.h`, and their old Telegram/CMakeLists entries,
are removed. All new implementation and registration live in Purple files and
Telegram/cmake/purple.cmake. See [upstream_hooks.md](upstream_hooks.md).

### Lock completion generation

A lock request may cross only its own synchronous unlocked-to-locked policy
transition, with automation, every capability and all lock switches unchanged. The server records
that exact post-lock generation in a request-local scope around the main-thread
handler. It does not derive permission from the returned JSON. Any later
generation invalidates the completion, even if a disabled switch has been
re-enabled or the policy values are unchanged: the reply uses the current
authorization error, or `busy` when the request is authorized again.

## Subscriptions, protocol 1.1

`hello` advertises `version:1, minor:1`; wire envelopes retain `v:1`.
`subscribe {"topics":["media.state","lock.state"]}` replaces this connection's
entire topic set atomically and replies with the accepted topics before any
initial snapshot. The set must contain one or two distinct supported topics.
Unknown topics or extra fields are rejected without changing an existing set.
`unsubscribe {}` removes all topics; disconnect, reconfigure and stop cancel them.
Clients that never subscribe receive only ordinary command replies.

Events are JSON Lines with `v`, `event` and either `result` or a typed `error`.
They have no request id or `ok` field. `media.state` is the same snapshot as
`media.now`, including downloaded-path verification and Work Mode withholding.
While passcode-locked it follows the same actual-type lock switches as
`media.now`. A refused snapshot emits only an error with code `locked`, without
ending the topic; a later unlock or switch change can resume snapshots. `lock.state` returns `{"locked":true|false}`. Other snapshot errors
end that topic with `ended:true`.

Media information and lock topics require the `media.info` and `lock` groups,
respectively. The master switch and topic capability are checked at admission
and every publication. Revocation ends affected topics with `ended:true` and
`automation-disabled` or `capability-off`, even if a setting is disabled and
re-enabled between worker slices. Re-enabling requires a new subscription.
Event authorization delegates to the same command-policy function as
`media.now` or `lock`, both when computing and immediately before sending.
Subscription admission permits the typed `locked` state without bypassing master
or capability checks. Epoch, client, topic serial, generation and revision fence
stale callbacks.
An event already written to the OS socket cannot be retracted; policy checks
apply immediately before adding bytes to the socket queue.

Each topic occupies one of the existing eight per-client and 64 total pending
slots. A topic retains only its latest bounded serialized event; the sum of all
staged events and Qt outbound bytes stays within 128 KiB, and each line within
64 KiB. Oversize output closes the connection. Notifications coalesce through
two atomic revision counters; a stalled transport stops new snapshot dispatch.
Each topic waits for its previous asynchronous operation to finish before
replacing it. An independent 64-operation lease also counts cancelled work
until completion or callback destruction, including across disconnects.
Snapshot/dispatch waits time out after ten monotonic seconds with a terminal
`busy` event. A terminal event blocked in the transport is retained for at most
another ten seconds before the connection is closed and its topic slots freed.
Already queued kernel bytes remain subject to the existing connection lifecycle.

Player subscriptions bind through `Purple::OnMediaPlayerStarted()` immediately
after upstream creates the player. The earlier app-service hook starts the
control listener before player creation, so binding signals there is unsafe.
The hook itself is one call in `core/application.cpp`; all event logic stays
in Purple sources. Host fixtures use a thin adapter and private sockets, not
real playback or a live profile.

`tdesktop-cli watch [media|lock|all] [--json]` defaults to both topics and prints
one line per event until interrupted. JSON mode prints the complete event;
human mode prefixes the topic and formats the snapshot on one line. A media
`locked` event leaves the watch running. Any terminal topic error prints its
reason and exits with the existing error code, closing the remaining topics.
Subscription acknowledgement uses the ordinary two-second deadline, then an
idle watch has no read deadline. Interrupt/SIGTERM closes the socket and exits0.

### Lock policy qualification

`control_media_cases.cpp` covers every policy unit with both lock and switch
values, owner defaults, explicit and implicit hidden voice, send-time lock/unlock
and switch races, and no RPC settings path. `control_subscription_cases.cpp`
covers retained music/voice subscriptions through lock, unlock and switch changes.
`control_emoji_cases.cpp` checks the extracted production demand and lock handlers
with a real worker, including default continuation, an existing published image
and cancellation when the switch is disabled. Its session shell is synthetic;
real account/player/passcode integration is compile-checked without launching an
app or a live profile. `control_preferences_cases.cpp` exercises the production
boolean KV serialization, missing-key defaults and persistence of each switch.

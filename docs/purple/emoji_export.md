# Custom emoji artwork for local tools

Turn on **Export custom emoji for local tools** in Purple settings to keep a
local emoji library catalog and transparent 128 px PNG first frames. It is off
by default and applies only to this installation. These files contain emoji
library metadata and artwork, so other local applications can read them.

On the default working directory the catalog is
`~/Library/Caches/Purple/emoji-export/catalog.json`. A different working directory
uses `<workdir>/emoji-export/catalog.json`, keeping test profiles separate.
The setting displays the actual directory; callers cannot choose another one.

The version 1 JSON envelope has `version`, UTC ISO `generatedAt`, and `entries`.
Each entry has decimal-string `id` and `setId`, `alt`, `setTitle`, `setShortName`,
`textColor`, positive `recentRank` or null, and relative `png` such as
`png/123-128.png`. Recent custom emoji come first in client order, followed by
installed emoji sets in client order; duplicate document IDs appear once.
Unavailable set metadata stays empty. Text-colour templates use the current
application foreground colour and refresh when its palette changes.

The client refreshes after session startup, recent emoji or installed emoji-set
changes, and palette changes, with a five-second debounce. Document resolution,
media loading and rendering are serialized. Media loading has no sticker-set or
message origin, so an expired file reference cannot trigger set or message reads.
Only custom-emoji document and media reads are made, with no message content
access and no sends. Both recent and installed candidates share a 100,000-entry
cap, plus one extra document ID. That extra ID gets reserved resolution admission
without changing catalog order. Each slice permits 100 document resolutions. A
250 ms internal continuation keeps the cursor and finishes remaining candidates
without another refresh event. Permanent resolution failures, media loads and decodes have
a one-minute cooldown, retained across refreshes, followed by an automatic retry.
Failures advance the cursor so a failing prefix cannot starve later candidates.
Document metadata requests use the shared request controller. One short flood
wait retries the same request once at X+1 seconds, extending its 30-second
response deadline only by that wait. A long or repeated flood, server/transport
failure or response timeout is handed back as a transient with retryAt. It does
not mark that document as failed. The queue projects retryAt from Unix time to
its elapsed clock without shortening the original monotonic wait at a wall-clock
second boundary, and pauses all metadata resolution until that deadline,
retaining the hold across refreshes. Cached artwork can still finish. The
existing automatic retry scheduler observes this deadline; cancellation retires
the controller before the handoff, so two retry timers cannot compete. Permanent
document errors retain the one-minute per-document cooldown. Media loads keep
their independent 30-second wait. Unsupported or oversized images
are omitted. Existing shared UI downloads are allowed to settle before another
export download starts; the exporter does not cancel a download the UI may use.
While a timed-out shared transfer settles, the pass can still render cached
artwork and skips other unavailable images. Its completion schedules a refresh.

Each PNG is replaced atomically before the catalog is replaced atomically.
At publication, catalog entries reference existing PNGs. A consumer retaining
an old catalog across replacement must reload it or tolerate a missing old PNG. Owned PNG files absent from the
new catalog are removed afterwards; unrelated files are preserved. A storage
failure revokes the output and stops that exporter until the setting is toggled
or the primary session changes. Symlink export directories are refused.

A single bounded worker performs decoding, PNG encoding and writes, catalog
validation and JSON publication, stale scanning and removal. It accepts at most
one active job and one pending job. All output work carries a generation token.
Disabling the setting, passcode locking, logging out or switching the primary
account immediately revokes publication authority and queues removal of the
catalog and owned PNGs behind any active worker. A new session waits for that
cleanup barrier. A later ordinary quit preserves any earlier cleanup obligation. An ordinary application quit keeps
the last catalog for offline local tools while stopping publication. No rendering
completion from a revoked generation may publish. The export follows the application's primary
(active) session, including when several accounts are signed in. There is no
manual control/socket command, and a chooser only needs to read these files.

Host fixtures are in `Telegram/SourceFiles/purple/tests/emoji_export_cases.cpp`
and `emoji_export_async_cases.cpp`. The latter uses the production queue and
worker, synthetic files and a Qt event loop to check overflow, failure fairness,
automatic continuation, generation revocation and large-library heartbeats.
`emoji_export_shutdown_cases.cpp` and `emoji_export_shutdown_cv.h` force an idle
worker into the gap between a false predicate and the underlying condition wait.
The fixture compiles the real worker TU against a header overlay replacing only
its condition-variable type with that seam; production headers stay unchanged.
Shutdown publishes its stopping predicate under the same mutex, then unlocks,
notifies and joins, so it cannot lose that wakeup.

## Upstream hooks

`main/main_account.cpp` calls a Purple logout hook before both ordinary and
forced logout, immediately revoking output and any publication authority for
that login. Startup and settings registration use existing Purple-owned hooks.

The transient regression is `purple/test_sync_retry_emoji.cpp`, run by
`python3 purple/test_sync_retry.py --output-dir <private-host-directory>`.
It executes the production controller and exporter handoff with injected network
and clock endpoints, plus literal queue code with Qt integer-type aliases. It
checks short and premium floods, long waits, 5xx, transport errors, own timeout,
unchanged permanent errors, global metadata holds, clock conversion and revoked
or superseded callbacks. The full global inventory admits this request only as
a managed SyncRequestSender site; raw MTP::Sender remains forbidden there.

An expired per-document cooldown does not outrank an active global metadata
wait. The queue's next deadline is the global deadline unless cached or media
work has an earlier future retry. The publication scheduler retains its bounded
one-minute refresh tick during long waits; expired metadata cooldowns cannot
turn it into repeated one-second catalog publications.

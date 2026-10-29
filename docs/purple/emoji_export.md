# Custom emoji artwork for local tools

Turn on **Export custom emoji for local tools** in Purple settings to keep a
local emoji library catalog, transparent 128 px PNG previews and larger first frames for local choosers. It is off
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
`installed` is an optional boolean, true only when the set is in the account's
installed emoji-set order. Other optional fields are `sourceKind` (`tgs`, `webm`
or `webp`), `sourcePx` (the longest native/rendered side), and `large`, a relative
path such as `large/123-512.png` when the large file is available. Version 1 and
the existing fields stay unchanged. Old consumers can ignore these additions.
Unavailable set metadata stays empty. Text-colour templates use the current
application foreground colour and refresh when its palette changes.

The client refreshes after session startup, recent emoji or installed emoji-set
changes, and palette changes, with a five-second debounce. Document resolution,
media loading and rendering are serialized. Media loading has no sticker-set or
message origin, so an expired file reference cannot trigger set or message reads.
The exporter reads custom-emoji documents and media, plus the title and short
name of an unknown emoji set through a read-only `messages.getStickerSet` with
its exact set ID and access hash. It does not feed that response into the
stickers model, install a set, or change archived or featured state. Installed
sets make no title request. Successful titles are cached per set for this
session and fill every matching entry; an unavailable title remains empty with
a one-minute cooldown. Document and set-title requests share one serial request
controller and its global transient deadline. There is no message content access
and no send. Both recent and installed candidates share a 100,000-entry
cap, plus one extra document ID and at most 1,024 remembered chooser-request IDs. That extra ID gets reserved resolution admission
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
new catalog are removed afterwards in pages of at most 256 directory entries per
publication; subsequent publications continue the scan. Unrelated files are preserved. A storage
failure revokes the output and stops that exporter until the setting is toggled
or the primary session changes. Symlink export directories are refused.

The export directory is in the user's own space. Processes running as that same
user are trusted: they can already write these files directly. Before each
explicit rename or removal, the worker rechecks that the target's parent is a
non-link directory with the same filesystem identity it verified earlier. A
changed parent skips the operation. This cheap check detects replacement during
encoding or a queued operation; it does not claim directory-handle confinement
against a malicious same-user process racing the final system call.

A single bounded worker performs decoding, PNG encoding and writes, catalog
validation and JSON publication, stale scanning and removal. It accepts at most
one active job and one pending job. All output work carries a generation token.
Disabling the setting, passcode locking, logging out, changing the palette or switching the primary
account immediately revokes publication authority and queues removal of the
catalog, owned previews and large PNGs, and empty regular request-folder files
behind any active worker. Cleanup enumerates at most 256 entries per worker
slice with constant retained state, checks revocation between entries and yields
between slices. A new session waits for the complete cleanup barrier. Before
cleanup starts the worker atomically records `.cleanup-pending`; it removes that
marker only after all pages complete. Quit cancels traversal and records an
outstanding obligation without walking the directories. On the next start,
recorded cleanup completes before any new export work. An ordinary application quit keeps
the last catalog for offline local tools while stopping publication. No rendering
completion from a revoked generation may publish. The export follows the application's primary
(active) session, including when several accounts are signed in. A chooser reads the catalog and can request a large render through the control socket
or the bounded file protocol below.

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

## Large artwork and chooser requests

The first 50 distinct recent custom emoji are rendered ahead of time. Ordinary
emoji and duplicate recent IDs do not consume those 50 slots. TGS vectors render
at 512 by 512. WebP and WebM render at their encoded native dimensions, never
upscaled; `sourcePx` is their longest side. Frames preserve alpha and text-colour
templates use the same foreground tint as the previews. Unsupported input or
native sides above 512 have no large output. Non-square video with a quarter-turn
rotation keeps its preview but has no large output because the upstream frame
generator cannot safely render that native rectangle.

To request another ID, create an **empty regular file** at
`<export>/requests/<id>`. The filename is the canonical decimal representation
of a nonzero unsigned 64-bit ID: ASCII digits only, no leading zero, sign,
whitespace, extension or path component. Do not use a symlink or write any
contents. Repeated requests for the same ID coalesce.

The existing one-second timer schedules worker scans, rather than adding a
filesystem watcher or reading the directory on the GUI thread. Each scan
examines at most 256 directory entries and admits at most 16 valid names. A
persistent iterator advances beyond invalid prefixes and lives on the worker.
The folder has a 1,024-entry admission ceiling, counting all entry types. Counting
is paged before admission; a completed count is a snapshot, so a concurrent
producer can temporarily exceed it. Once an overflow is observed, further
requests are ignored until a full scan sees at most 1,024 entries. During overflow,
each page removes its oldest empty regular files with malformed names first
(modification time, then filename), at most 256 per page. Canonical requests,
nonempty files, directories and symlinks are preserved. A folder filled with
these preserved entries needs its producer to remove them before admission can
resume. No payload is read, and at most one page of malformed metadata is kept.
A successful demand is acknowledged in the publication worker job, after the
catalog advertises its large path and that path is checked again as an existing
nonempty regular file. If the rendered file is missing, the request remains for
a later retry; render completion alone does not acknowledge it.

There are at most 64 queued or active requests, with at least 250 ms between
dispatches. A full queue leaves files for later admission. Up to 1,024 demanded
IDs are remembered for subsequent refreshes. Later unknown IDs reuse old demand
slots and retire their old catalog references without growing session memory. Work already in the ordinary queue keeps its
cursor, failure cooldowns, request budget and global Telegram wait.

A successful request atomically writes `large/<id>-<sourcePx>.png`, publishes its
catalog reference, and then removes the request file on the worker. Partial
publication retains the rest of the previous catalog while the ordinary pass
continues. An unsupported or unresolvable ID is dropped with the normal bounded
failure cooldown. A Telegram transient retains the request and resumes it after
the shared monotonic deadline without another chooser event. Acknowledgement
only removes a canonical, empty regular request file. Malformed empty files are preserved below the folder ceiling and discarded on
overflow or revocation. Nonempty files, symlinks and directories are left alone.

**Large emoji cache limit**, beside the export toggle, defaults to 2 GiB and
provides choices from 100 MiB to 8 GiB. This installation-only value uses the
generic preferences store. Lowering it gives pruning priority at the worker's
next safe point, after any active job. Only owned large PNG bytes count toward
the cap; previews and the catalog do not. Least recently used files are removed
first, using their modification time, refreshed when a request renders them
again. Pruning examines and removes at most 256 entries per step, advances on
the one-second maintenance tick, and retains at most 2,048 owned-file records.
This separate file-count cap includes zero-byte files. If a scan finds more,
a bounded min-heap retains the 2,048 most recently modified candidates, replacing
the oldest retained file whenever a newer one appears later in the scan. Equal
timestamps use filename order. A modification time more than five minutes after
the current wall clock is implausible and ranks as the oldest, including for the
file-count cap. Exactly five minutes ahead remains plausible. Implausible times
tie by filename rather than their raw future value. Each worker step captures
one wall-clock value and rebuilds the bounded heap under it, so clock corrections
between pages cannot leave an inconsistent ordering. A current demand writes or
refreshes its file to the current time and outranks every implausibly future
candidate. Writes and touches refresh retained metadata, so
an in-progress scan respects newly demanded artwork. Byte-cap eviction also
removes the oldest retained file first. A cap change restarts the bounded
scan, and further pages continue without requiring another setting change.
Catalog retraction accepts at most 64 MiB and 101,025 entries, matching the
candidate ceiling; larger or malformed catalogs fail closed. Their catalog references are retracted atomically before removal, and
subsequent publications validate file existence so old snapshots cannot restore
those references. A consumer holding an older catalog should reload it and
tolerate a missing file after pruning or revocation.

`python3 purple/test_emoji_large.py --build-dir <Debug build> --output-dir <dir>`
runs the policies, store and actual-worker fixtures, real TGS/WebP/WebM rendering,
and literal exporter admission, metadata, maintenance, recent selection and
preference callbacks. `emoji_large_bounds_cases.cpp` checks 6,000 zero-byte
large files, 6,000 malformed requests, paged revocation, replaced parent
identities, and shutdown/restart with 5,000 pending files.
`emoji_large_demand_cases.cpp` fills the old cohort before a late new render,
checks normal and future-dated full cohorts, and ties acknowledgement to an
advertised existing image. Exact five-minute and one-millisecond-beyond times,
equal mtimes and per-step/index limits run on the production worker.
It also retains a request whose rendered file vanished. All nine programs are registered as the
`test_emoji_large` configuration in the canonical host gate list. The existing
emoji export, async and shutdown configurations remain required.

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

## Socket demand

When Local automation and its emoji capability are enabled, `emoji.render`
uses the same Large demand queue and renderer, with bounded asynchronous replies
and optional downscaling. A demand admitted while an ordinary preview-only job is
running waits for its own Large render; a successful preview does not reject that
waiter merely because it did not request Large output. See [the control socket guide](control_socket.md#emoji-rendering)
for arguments, errors and CLI examples. Request files remain the fallback for
consumers that cannot reach the running app's control socket.

# Local message archive

The archive keeps observations separately for each signed-in account and local
profile. Payloads, body chunks, media chunks and keys are authenticated and
encrypted. SQLite indexes contain keyed identity tokens and structural counters.
Those indexes select candidates; authenticated rows establish account ownership
and message identity. This is local retention, not a Telegram cloud backup.

The storage adapter uses the committed purple-core archive contract. A private,
symbol-prefixed SQLite amalgamation is pinned under
`Telegram/ThirdParty/purple_sqlite`. `VERSION.json` records the official download,
published SHA3 and individual SHA256 hashes; `generate_prefix.py` verifies the
pristine files before regenerating the prefix map. This target does not use
QtSql, SQLite extensions or the platform SQLite library.

Each native profile directory has a random profile binding, with a private
recovery link under the archive root. Each stable account has its own store
incarnation, random data key, process lock and database. HKDF-SHA256 derives a
wrapping key from the existing random local-storage key and a separate index
key from the archive data key. Passcode changes keep the underlying local key.
A missing or unwrappable old key refuses the archive. It never creates a new key
over an existing store. A copied profile binding whose native directory identity
changed also refuses instead of silently sharing another profile's archive.

The macOS adapter excludes owned directories and files from system backups and
checks the result. Shared profile/catalogue publication is serialized across
account workers, so simultaneous sign-ins cannot lose another account entry.
Private files must belong to the current user, have no other
user permissions, and have exactly one link. Symlinks refuse. Windows has a
separate protected owner-only DACL path; its native behavior still requires
Windows qualification. Parent directories are synced after publication/removal.

All SQL preparation, binding, stepping, finalization, row/change expectations,
commit and close results are checked. An operation acknowledges retention only
after the exact core program commits. WAL, foreign keys and FULL synchronous
mode are set and read back. Every connection is admitted before configuration writes: read the full-width
format first, then the recorded fingerprint and all bounded noninternal DDL in
one read snapshot. Format 2 requires the exact core shape. Format-1 drafts and
fingerprint/DDL drift refuse as SchemaMismatch; future formats refuse ReadOnly.
There is no migration or automatic discard. Explicit account-store removal and
recreation is required to replace an incompatible draft.
Version payloads above 16 KiB use authenticated SQLite-owned body chunks. A
version above the core 4 MiB encoded limit records a retention gap instead of
claiming success. History reads use bounded pages and a revision fence; a changed
revision requires refreshing the view rather than combining different snapshots.

Media copies consume already available bytes; this adapter makes no download
requests. Media uses owned 1 MiB encrypted chunks with no arbitrary attachment
size limit or expiry. A copy must match its exact known length and whole SHA256,
be fully checked, and be durably published before its database reference becomes
Complete. Reader leases pin blobs and every chunk checks the current generation.
Extra, missing, truncated or unauthenticated chunks refuse. Failed staging and
unreferenced publication remain owned and count toward physical usage until
reconciliation or logout removes them.

Logout never waits for or depends on archive IO. Normal and forced logout hooks
revoke archive capability before upstream continues. They clear the per-login
account preferences, cancel queued work and pending deliveries, and wipe the
shared wrapping, data, index and admission keys. SQLite admission closes at
cancellation; its owner thread closes the handle after any active call returns.
No main-thread future, join or retry loop participates in logout.

Archive admission binds a random 32-byte per-login value to the sorted set of
persistent MTP key IDs across all DCs, excluding temporary and local keys. The
current main key is a wrapping selection, not the login identity. The native
account KV stores the nonce and the complete key set; the nonce survives iff
the stored set intersects the live persistent set. A disjoint live set generates
a fresh nonce. Both fields must be verified in persisted read-only TDF/KV
snapshots before opening; failed publication holds instead of falling back.

Admission version 3 has exactly eleven checked, authenticated fields: store,
key, generation, phase, slot, loginAuthority, loginAuthorityVersion,
loginAuthKeyId, loginAuthKeys, wrappedKey and loginAdmissionMac. Missing,
unknown, extra or malformed fields, unsupported versions, wrong local keys and
bad MACs refuse before any orphan decision. Earlier drafts have no implicit
migration or automatic discard; use the explicit per-account action below.

A valid record is the same login when at least one of its bound persistent IDs
is live and its nonce hash matches. Its secret is unwrapped using the old
record's authenticated set and main selection. Whenever the live set or main
selection differs, a new wrapping file is written durably first, then the whole
signed record is atomically published with its new file reference. The previous
wrap is retained. Interrupted or refused publication therefore leaves an old or
new complete valid binding, rather than a record pointing to an unwritten wrap.
Store incarnation, data key, epochs and retained versions do not change.
Unreferenced wrapping files remain owned and are removed with the store.
Rebinding runs on every open where the set or selection differs. It does not
run in an already active worker; an authorized runtime change is absorbed on
the next open/restart. This avoids an additional asynchronous writer transition.

Each login-anchor case is safe for these reasons:

- USER_MIGRATE changes the main DC without logging out. The all-DC set still
  intersects, so the nonce and archive survive and the next open rebinds them.
- Dropping a non-main key, or dropping the former main after rebind, leaves an
  intersection. The store is retained and the reduced set is published.
- Fresh login resets persistent authorization keys. A fully authenticated
  record with no live bound key is an orphan and is removed before empty Ready.
- Failed native reset followed by a new login cannot reuse stale KV: the new
  persistent set is disjoint, so selection rotates the nonce and native admission
  waits for that new pair. The authenticated old orphan is then removed.
- Nonce loss or change while any bound key is still live refuses without deleting.
  Lost KV is not proof of logout. Every other mismatch also refuses.

The persistent set is returned by mtproto/mtp_instance.h:89 and
mtproto/mtp_instance.cpp:847-854. AuthKey::keyId is exposed at
mtproto/mtproto_auth_key.h:36 and .cpp:38-40; it identifies persistent key
material, not a transport session. Fresh persistent DH material is installed by
mtproto/details/mtproto_dc_key_creator.cpp:829-839. Forced logout replaces the
MTP instance at main/main_account.cpp:543-547 and :628-637. Intro startup invokes
destroyStaleAuthorizationKeys at intro/intro_start.cpp:33 and
main/main_account.cpp:603-613 before fresh sign-in. Failed native reset can
reload stale upstream authorization; its next server rejection repeats revocation.

## Explicit per-account recovery

Purple settings always shows "Discard this account's archive and start fresh",
including for refused records, with a strong confirmation that all retained
messages, edit history and owned media for this account will be permanently
deleted. Refusal notices name the action. It does not modify Telegram chat data.

Confirmation captures the opening session, account, worker instance, nonce and
live key set. A switched window, newer login/worker, changed context or repeated
callback refuses. On confirmation the old worker is stopped, its keys and
pending deliveries are revoked immediately, and a background wait has a 500 ms
limit. The main thread never waits. Under the account lock, a discard token
captures the profile and account-directory identities, exact account record and
fence digest. Cleanup checks every fence again, and targets only the derived
account directory. It cannot follow a malformed record's store path into another
account or discard a newer incarnation. The existing removal state machine and
bounded pages perform deletion, with a 500 ms cooperative cleanup budget.
Cancellation on logout/session loss/key-set change stops further actions. Kernel
IO is not preemptible; it remains off the main thread.

Only completed cleanup permits a new random nonce and empty store. Failure stays
visibly held; invoking the action again obtains a fresh fenced continuation.
A missing or corrupt record can therefore recover without weakening automatic
admission. Unreadable profile/catalogue metadata that cannot be snapshotted
safely still refuses. Other accounts and unbound profile roots are never swept.

## Logout cleanup

A worker attempts keyless removal at logout, including when no capture worker
was running. It uses at most three attempts, 50/200 ms backoff, a 500 ms
cooperative IO budget and bounded removal pages. Persistent failure reports
pending cleanup and stops retries. Individual kernel IO calls cannot be
preempted; they never delay the main-thread logout. A durable intent is useful
for startup cleanup but is not the revocation authority. Missing intent under
total archive write failure cannot grant the old store access on re-login.
Unresolved orphan cleanup refuses Ready and retries on the next admission
opportunity. Cleanup is fenced to the old authority and exact store ID, so a
late worker cannot remove a newer login. A failed lock-file unlink is retried
only for this process's released lock with the exact native file identity.
Process-lifetime registry storage keeps self-owned worker cleanup safe during
shutdown. Quitting or switching accounts preserves the persisted authority.

The local-key and reset contract is verified in these upstream paths:
Storage::Domain::startAdded supplies the shared local key
(storage/storage_domain.cpp:77-83); Storage::Account::startAdded assigns it
(storage/storage_account.cpp:231-235). Storage::Account::reset clears _prefs
and _prefsKey (storage/storage_account.cpp:767-779), rewrites the map and
authorization data, and asynchronously removes collected old files
(storage/storage_account.cpp:820-839). It does not clear _localKey.
Both logout routes reach Account::loggedOut, session destruction and
local().reset in main/main_account.cpp. Exact current source locations are
also recorded in the external review-fix evidence.

Headless checks: `python3 purple/test_archive.py --build-dir <Debug tree>` runs
real adapters with temporary profiles. `--use-build-objects` uses the exact
production objects. On macOS these tests need backup-metadata access.
`ARCHIVE_SQLITE_TESTS=1 purple/test_config.sh` also runs the shared real-SQL suite.
Neither test opens an application window or accesses a signed-in profile.

The profile binding also includes Telegram's data-file namespace. Two `-key`
datasets in the same native `tdata` directory therefore get independent stores,
locks and wrapping contexts. A private native-directory recovery link survives
loss of just the profile marker without choosing a fresh empty archive.

## Windows durability

On Windows, regular-file flushing uses the write-access QSaveFile handle.
Directory metadata durability uses a checked writable volume handle, never a
directory FlushFileBuffers call. Windows requires administrative volume access
for this documented barrier; unavailable access refuses archive operations and
leaves a visible held state. The client does not request elevation or silently
claim a directory flush succeeded. Native Windows runtime qualification remains
pending. References: [FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)
and [volume handles](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew).

## Capture and edit history

Accepted edit/delete dispatch writes an encrypted, bounded outbox before
upstream mutation. File publication, file sync and parent-directory sync form
the write-ahead barrier. This is synchronous local IO of a bounded record, so a
slow disk can delay that barrier. Updates continue after a failed barrier with
a visible gap; they never wait for a database lock or queue retry. A generic
encrypted attention marker preserves queue/append failures when its small write
can succeed. Failure to retain even that marker remains visible for the session.

One dedicated writer thread per session drains at most 64 queued observations.
Its stable sequence is persisted before each entry and is replayed unchanged.
Accepted pts, message identity and semantic content distinguish edit identities,
including same-second A-to-B-to-A. Replay checks committed receipts before
retiring entries. A new retry does not recapture account or purge generations.
Matching invalid/stale observations hold or record a gap; cancelled sessions
cannot recreate a removed archive. Work and deliveries are revoked immediately at logout; queued work is
discarded and a self-owned worker attempts bounded account removal.

Baseline creation, current preimages and explicit accepted edits are distinct.
A live preimage supplies a partial baseline when there is no retained head.
A retained head avoids synthesizing another preimage from a less complete UI
representation. Changed unbound history responses and quality refreshes cannot
become authored edits or replace a newer head; they can leave an explicit gap.
Message views, reactions, file references, access hashes and alternative document
qualities remain in observation payloads, outside ordinary document/photo body
comparison. Rich page blocks retain semantic structure separately from their
transport catalogues. Unsupported structures are labelled opaque or partial.
Original source Markdown is never invented from rendered or received text.
Sender and reply parents retain typed identities and full-width string ids;
forum topic projection uses the actual parent, and album ids remain full-width.

Explicit channel deletes retain their full addressed identity. Non-channel
server IDs are resolved only through authenticated rows for this account's
original received messages, including evicted live messages. Missing or ambiguous
identity records a gap instead of deleting an arbitrary archive message.

Available document files/bytes and photo bytes can be copied by the existing
writer after observation. It issues no network or auto-download calls. Size,
whole hash, generation, owning descriptor and every encrypted chunk must agree
before a Complete reference commits. A cache source that disappears or changes
stays incomplete. Later lifecycle/download-completion hooks belong to M3B3.

View Edit History captures the opening session, account, chat and message. It
reads 32 versions at a time without reconstructing live HistoryItems. Cursor,
revision and store checks refuse mixed snapshots. Account-window replacement,
logout and view disposal cancel delivery. The current viewer is a selectable text
preview, limited to 8 KiB per version; it labels partial/opaque bodies and owned
attachment counts. Full structured bodies remain encrypted in storage. Native
rich rendering, attachment opening and accessibility/layout checks are not
qualified by the headless fixture.

Cached-file capture carries the registered file's native identity, size and
modification time. Opening verifies the descriptor itself, not just a pathname,
and hashing checks that descriptor again. A replaced path or changed file is not
accepted as the document's downloaded bytes. Immutable available memory bytes
are retained by value. UI page delivery also carries the worker operation ticket;
a newer operation or stop revokes an already queued response before rendering.

Media work retains the observation id and canonical content hash. It resolves an
authenticated historical version, never the app's newer head merely because a
media identity matches. Persisted jobs bind exact version, revision, content hash
and purge generation; completion also checks the captured external store epoch.
A valid historical copy can finish after a newer version arrives without filling
the newer reference or cancelling its independent job. Revision/hydration changes
cancel obsolete staging; old job ids cannot be rebound. Lifecycle events retain
the captured epoch/media binding through the encrypted outbox, and SQL capture
checks a fresh executor epoch. Dedicated expiry/download/view/purge UX remains
in the later batches; their new core APIs and format-2 projections are registered.

## Review failure boundaries

Runtime journal replay failure preserves its encrypted outbox evidence and
writes a durable attention marker where storage allows. The service reports a
visible RetentionGap with the replay reason instead of an empty Ready notice.
History pages expose the gap after restart as well.

Delayed already-available media lookup matches the authenticated historical
version ID or observation revision. It scans bounded authenticated version
pages under one revision fence, then checks exact content hash, media identity,
account and current epoch again. Plaintext index hints alone never select it.

Short send acknowledgements are captured only after local message-ID adoption
and acknowledged fields are applied. Their observed baseline remains Partial;
the short TL evidence is retained. Repeated acknowledgements enrich metadata
without inventing an authored edit.

Capture journal IO now uses the bounded handoff described under expiring-media retention.

## Windows non-admin follow-up

The checked writable-volume metadata barrier is a temporary capability refusal
for ordinary non-admin Windows users. It never delays or vetoes logout.
A separately reviewed non-admin atomic publication and recovery design remains
a follow-up. It must state and qualify the power-loss guarantees for rename and
deletion metadata; silently skipping the directory barrier is not equivalent.
No such design or Windows runtime qualification is included here.

## Expiring-media retention (Desktop M3B3)

The native TTL timer, destructive view-once consumption, expired-media replacement
and whole-message TTL queue a preimage and an explicit lifecycle fact before
Telegram destroys the item. Ordinary reads, cache eviction, unload and item removal
do not imply expiry. Desktop has no secret-chat transport and emits no Secret facts.
The retained original body stays distinguishable from Expired or Consumed state.
An expired protocol placeholder is intercepted before normal edit capture, so it
cannot replace an original with a fabricated empty edit.

A destructive seam waits at most 10 ms for a small encrypted journal write on a
separate writer thread (at most eight outstanding writers, bounded preimage).
The encoded lifecycle fact is checked against a 60 KiB limit before any journal
write. The 10 ms bound applies to waiting for the durable acknowledgement;
snapshot preparation and ordinary thread scheduling are additional work.
The main thread performs no new fsync or media-byte I/O and never waits for a
network request, media copying or archive-worker completion. If acknowledgement
misses the deadline, Telegram proceeds and the archive visibly records a retention
gap. This is a scheduling deadline rather than a real-time kernel guarantee.
Ordinary captures use the same bounded handoff. All journal Append, failure
inspection and Attention IO stay on background writers; no foreground capture
or error path takes the journal IO mutex. Admission is a nonblocking worker
queue plus at most eight capture writers. Busy/full admission or a missed
acknowledgement records a visible gap; admitted encrypted outbox records remain
replayable even when a drain notification cannot be queued. Gap tasks have a
small reserved queue allowance. Logout still revokes immediately without joining
these writers.
A file descriptor or buffered stream fragment is not claimed as crash-durable.
Only durably authenticated owned chunks and committed completion count as retained.

Acquisition observes existing document/photo completion notifications and passive
stream loader parts. It never calls wanted(), save(), load(), fill(), changes a
priority or creates a loader. A bounded observation registry binds completion to
its original account, message, observation, body hash and media identity. Alternative
video documents are distinct captured identities. A missing binding reports a gap.
Every separately validated origin is registered when a Reader or cached streaming
Document is reused. Cached Documents keep a weak association with their actual
Reader, so another reader-map entry cannot redirect that binding. Bindings are
idempotent for one observation and limited to 64 origins per Reader, 64 live
Readers and 64 cached Document associations per session. Expired weak references
are pruned. New passive parts feed each exact immutable observation/version;
no additional loader or request is created. If a new binding arrives after parts
already passed, the adapter explicitly records a retention gap instead of claiming
those bytes were retained or reading playback cache. Unbound origins, overflow,
missing Document association and loader failure also report gaps.

A completion notification alone is not proof: memory/file snapshots require exact
size and native descriptor identity and are hashed/copied on the account worker.

Passive fragments use core purple_archive_staging format 1. Each job has one
account-owned staging directory named by its keyed binding token. The directory
owns all provisional images, superseded images, progress and SQLite sidecars,
including unpublished orphans. A private native SQLite catalogue atomically CASes
the encrypted core header, encrypted page/delta and untrusted Merkle node hints.
This catalogue is separate from the format-2 archive database and changes none
of its schema. Every statement uses the checked private SQLite runner and exact
row/change counts. Native indexes never authorize content; core authenticates
membership/absence proofs against the header. Frames are bounded before reading.
No message body, attachment bytes or unkeyed attachment hash is stored in plaintext.

Fragments are split at 256 KiB windows; progress and proofs stay bounded regardless
of attachment size. Out-of-order delivery and duplicate bytes preserve coverage;
conflicting overlap refuses. Only a durable piece plus atomic catalogue commit
credits bytes. Complete coverage freezes an encrypted checkpoint, then core's
two-pass finalizer authenticates coverage and whole hash before resealing canonical
1 MiB chunks. Publication is checked against the exact job/version/revision/hash,
purge generations and external epoch. It can complete a retained historical version
without filling a newer head. Physical usage includes all staging and publication
orphans. Startup scans jobs in bounded pages and resumes complete frozen staging;
partial staging remains explicitly incomplete until more already-acquired bytes
arrive. Logout and confirmed per-account discard revoke the same workers, secrets,
queued parts and readers and remove the complete owned directory tree.

New and reused staging pieces are reopened and authenticated before coverage
credit. Canonical publication acknowledgements use the actual reopened frame.
Missing/tampered pieces and progress headers refuse completion. A repeated part
after completion verifies the owned historical bytes instead of creating a false
gap. Destruction with media that is not owned records a visible durable gap;
the original body and lifecycle state remain available.

## Owned attachment Open and Save (Desktop M3B3)

View Edit History offers attachment actions for each retained version's Complete
references. They address the archived account/message/version/media identity,
not a live HistoryItem or the latest message head. Opening acquires a core reader
lease and binds its revision, content hash, blob, store epoch and purge generations.
The action carries that tuple from its displayed page into the executor; it never
adopts a fresh store or revision at click time. Maintenance is unavailable rather
than an empty history. Page refusal removes the stale attachment actions.
There are at most sixteen native readers. Every range and delivery rechecks that
capability. A new head does not invalidate an exact historical version; a changed
revision, purge, source/Work Mode choice, session, logout or discard does.

The viewer uses an archive-only streaming loader with no cache key or shared cache.
It requests at most eight 128 KiB parts concurrently, never more than 1 MiB per
range. File-thread requests are marshalled onto the loader's Qt thread before
checking UI authority. Overlapping seeks queue offsets and prioritize fresh demand;
queued metadata is limited by the native 32-bit offset domain (at most 32,768 parts).
Cancellation tickets discard old seek completions without cancelling newer
requests. Playback uses Telegram's existing decoder with widget-sized frame
conversion. Pause/Resume and Play from start are available for supported audio/video.
Native Reader uses 32-bit offsets, so previews above its approximately 4 GiB bound
refuse; Save remains available without an archive attachment-size cap. Photo previews
are limited to 16 MiB encoded and eight million pixels, decoded off the main thread.
Unsupported formats and sizes are labelled explicitly and retain Save.
Archive readers opt into uncached slice eviction. The existing two-slice memory
budget applies as soon as a range is requested, including before its bytes arrive.
Late delivery to a retired slice is dropped and a later seek can request it again.
No plaintext is serialized into the shared cache. The preview size check leaves
a 16 MiB arithmetic margin below UINT32_MAX
for native slice rounding and preload offsets. Other reader callers retain their
existing default behavior.

Save requires a user-chosen destination. The account worker authenticates the blob
manifest/chunks and complete size/hash while streaming an atomic QSaveFile export.
Cancellation, corruption or authority loss before publication drops the partial
write. A stale completion is not reported as confirmed success. The viewer closes
on origin changes and revokes access before accepting another result. Canonical
parent checks reject destinations inside the archive, including directory aliases.
Plaintext is bounded playback memory or the explicitly requested export, never a
hidden temporary export for another application. No network acquisition is started.

Close, error and stale callbacks revoke their own capability and queue lease
release. Failed release keeps its checkpoint and GC pin, with a visible held reason;
subsequent store progress retries it. Active maintenance is not mistaken for an
absent lease: retirement requires a positive exact-id absence proof with normal
admission. A purge can hold release until its lease phase has advanced. Logout and
account discard revoke all handles and delete the owned store, including staging.
The dedicated Deleted messages view and purge executor remain the next batch.

Headless fixtures exercise the actual native encrypted store, range/export IO,
endpoint, loader and service callback bodies with controlled executor scheduling.
They do not qualify native dialog layout, accessibility or real audio/video playback.

## History pagination and attachment leases

Opening and closing an attachment advances only its owning History box's
revision fence, only after a confirmed lease commit. The adapter compares the
actual expected and next states with leases and revision cleared, and requires
an exact single revision step from that box's current revision. Other writes,
interleaved capture or purge, another box, a failed or unknown commit, and a
changed account, login, store or page keep the old fence. Older then refuses a
stale snapshot. Closing the attachment still reports its release to a live
owning History box; closing or refreshing that History box drops the delivery.

The access fixture covers 41 versions with a 32-row first page, nine older
versions after an own Open/close, and unchanged, payload, purge, interleaved
capture and release-failure controls. These cases exercise the native encrypted
adapter and the production media completion callback with a controlled main
queue. They do not launch a GUI or qualify native window activation events.

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
The dedicated Deleted messages view and local purge actions are described below.

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


### Streaming staging cost and connection lifetime

The passive loader's GUI callback admits parts using only a memory flag. The
worker checks the durable login/store fence before resolving or writing a part.
Catalogue SQL checks memory liveness per statement and row. Each bounded staging
step authenticates its addressed generation, and fresh durable authority is
checked immediately before publishing bytes and before catalogue COMMIT. These
checks are not cached across steps. A refused COMMIT rolls back its transaction.

The 46 Merkle siblings are read in one bounded query. Missing siblings remain
empty hints; the core authenticates their proof against the sealed header.
Returned levels, positions, hashes, uniqueness and counts are checked.

At most eight catalogues remain open between callbacks. Reuse binds to the full
authenticated staging id and that sink's lifetime. Each step checks the directory
and catalogue identities. A primary mutation, purge, logout, discard, completion
or failed stream closes cached catalogues. Reader destruction revokes a memory
flag and sets a coalesced worker wakeup flag. This requires neither a queue slot
nor a GUI-thread mutex; each queued task also prunes retired streams. No GUI destructor waits for disk or worker completion. Eviction
does not remove durable staging bytes; normal authenticated recovery still runs.

The lifecycle fixture counts actual fence reads, catalogue opens and proof queries
for 2 MiB inputs in 64/128 KiB arrivals, including finalization. It requires at
least tenfold reductions in fence/proof counts and one open. The passive fixture
runs the literal production callback and requires zero GUI fence reads. Existing
crash, torn-source, logout/discard and stale-generation cases remain in place.

Follow-up: every arrival still reseals its full 256 KiB window. Changing that
requires its own durable publication and crash-consistency design and review.

## Deleted messages view

The chat menu opens a dedicated read-only Deleted messages box. It never inserts
snapshots into Telegram history, dialogs, notifications, search or forwarding.
The ordinary scope is Received/original for the addressed parent and explicit
migration links published by that account. A topic menu filters that actual
parent topic; the same numeric topic in a migration alias is a different scope.
Account, typed parent, aliases, topic, representation, opening login/controller,
Work Mode and app-wide source remain fenced across every asynchronous boundary.

The account worker owns a separate read-only SQLite connection for each view.
It admits format and actual DDL in the same read transaction used for all pages,
authenticates shell/head frames against returned row identities, and restores
body chunks through that connection. A missing authenticated head or body is a
visible refusal, never an empty tombstone fallback. Chronological/reverse pages
use all three cursor fields, with limits of 1 through 128. The UI retains one
32-row page. Later writes can offer Refresh without changing its snapshot.

Views share the existing 16-reader budget with media access. A 30-second idle
expiry closes them on the worker's five-second idle sweep; the UI independently
clears expired content and actions. Refresh/disposal posts bounded cleanup and
revokes delivery immediately. Logout revokes every published view flag at once,
and the worker closes all read connections before writer close or removal.
No GUI thread waits for a journal, database, media operation or fsync.

Counts use the seven checked int64 projections; admission/query failure displays
Unavailable rather than zero. Logical retained size scans at most 128 unique
referenced blobs per request, authenticates Complete frames and canonical uint64
sizes, and checks summation overflow. Continue advances an incomplete scan.
Logical bytes include the scoped retained archive, not only deleted bodies.
Physical database/WAL/SHM/staging usage remains separate; neither value predicts
space freed by a purge. Bodyless tombstones say no body was captured. Badges and
times describe observed deletion/expiry/consumption, without inventing an actor,
exact server deletion time or Markdown source. History and owned Open/Save carry
the full authenticated row key and media generations from the displayed page.

`purple/test_archive.py --views --use-build-objects` qualifies the real private
SQLite/crypto/body/query adapters and the literal scope comparator on temporary
stores. Native menus, layout, accessibility and playback need separate native
qualification; no live profile is required for these fixtures.

The deleted-view worker also prepares bounded text excerpts and computes each
head content hash once off-main. Full authenticated heads and row-wrapper
metadata remain in the query result for exact history/media capabilities;
attachment buttons retain only the small request and revocation flag.

Committed writes validate each deleted view's live generation and maintenance
admission inside Execute, before returning to the worker dispatcher. An affected
view's shared active flag is cleared at that boundary, so queued pages and
already displayed actions refuse immediately. Ordinary writes retain the
independent snapshot and can offer Refresh. Validation continues through all
views even if one connection fails to close. Expiry sweeps run between busy
tasks as well as during idle periods and remain a backstop. The signed-id
fixture uses an explicitly synthetic core Secret namespace only to qualify
storage tuples; Desktop still captures no secret-chat transport evidence.

## Account and chat archive removal

Empty Trashbin and Clear Edit History are available from the chat archive menu,
Deleted messages view and the opening account's Purple Settings archive section.
The settings scope is explicitly that named account, across all its chats.
A chat action names its parent and trusted migration aliases. It covers all
of their topics, collections and representations, even when opened from a
filtered topic view. This never removes Telegram messages, ordinary cache files,
ratings, pins, live history, unread state or notifications.

Empty Trashbin removes every retained version of deleted, expired or consumed
messages, keeping non-content tombstones and receipts. Clear Edit History keeps
the exact latest captured body, including a deleted message's latest body.
Counts and the core confirmation come from one authenticated header/epoch read
snapshot. Confirmation retains the opening controller, login, account, Work Mode,
app-wide source, exact typed scope, kind and revision. A changed context or write
requires a new review; neither the destination nor its authority is recaptured
from a newly selected account. Retry continues an already committed operation's
original scope and is fenced to the account's reviewed store epoch.

The worker pauses new capture admission before draining pre-existing durable
observations. The core Begin transaction checks feasibility before writing its
sealed operation journal, generation changes or access gate. A failed Begin
needs an affirmative authenticated absence read before releasing its capture
claim; an ambiguous result holds visibly. No main-thread IO wait is added.
After commit, closing the dialog stops its deliveries, while maintenance keeps
running. Logout still revokes and proceeds immediately.

Startup admits an active journal before any ordinary content query, capture,
stream recovery or abandoned-reader release. It authenticates the operation and
checks its kind, scope token, phase, cursor, revision and account generation
against SQL metadata. Continuation reads at most 128 compact rows. Body references
remain intact for the core's exact original-frame checks. Each incomplete target
supplies all five columns of authenticated owned-copy evidence. The adapter
bounds gathered evidence, and the core may commit a smaller leading prefix.
Only that prefix's persisted cancellations and revocations can drive cleanup.
Surviving media is reconciled before message/version removal. Every phase and
cursor advances durably, including SQL exhaustion proof for an empty page.
Failure keeps the journal and a visible held status; Retry resumes it, without
an automatic infinite retry loop.

Begin revokes view/media delivery at once; revoked leases keep their pins until
release. Finish renews capture authority with an authenticated per-account/chat
generation snapshot. Old queued facts keep their captured generations. An
unresolved non-channel delete carries its immutable captured generation evidence
in the encrypted outbox, so later key resolution never borrows newer authority.
Publication admits at most 4096 chat-generation facts, read in pages of 128;
missing, malformed or excessive evidence holds capture with a visible gap rather
than assuming generation zero. The main thread reads only the published memory
snapshot. Old stream callbacks stay paused after a committed purge; acquisition
observations are retired before new journal authority is published. Fresh server
observations after Finish can capture and stage new media. A capability/access
gap is disclosed and, when possible, retained as an encrypted attention record.

Garbage collection is serialized with owned publication on the same worker.
The core commits GarbagePending after checking references and leases. The
adapter then authenticates that exact pending blob and absence of pins before
removing only its addressed owned directory, at most 64 traversal actions per
step. Missing owned files are idempotent cleanup. Complete removes metadata only
after acknowledged file removal, with pins checked again. Pending copies cannot
be overwritten or revived; new streaming bytes stay staged until reconciliation
allows publication. Cancelled stream directories are derived from authenticated
job bindings and removed in bounded pages; ordinary cache paths are never used.
Logical retained size, physical owned usage and bytes actually freed differ.

`purple/test_archive.py --purge --use-build-objects` drives the real native store,
maintenance engine, worker and literal execution callback with controlled UI
routing and a controlled main-queue dispatcher. It includes stale confirmations,
131-row scopes, typed aliases and message forms, shared owned media, body chunks,
restart cuts, failed commits, malformed cursors, generation renewal, lease pins,
bounded interrupted GC and lost-authority cleanup. Native presentation and live
account events require separate qualification.

### Purge commit boundary

All maintenance SQL commits, including Begin, bounded continuation, marking
GarbagePending and acknowledging physical deletion, use the store's Execute
boundary. Maintenance helpers require a commit delegate and have no direct
SQLite execution fallback. Only continuation receives the private admission for
an authenticated pending maintenance run; ordinary writes remain refused.
The shared boundary validates deleted snapshots and media capabilities before
returning to the worker dispatcher. The continuation preserves revoked media
pins until Finish, rather than attempting ordinary lease releases while the
authenticated maintenance gate is closed. A close failure holds visibly and
revokes the remaining capabilities.

The purge fixture includes refused delegates and a failed real COMMIT followed
by a successful Begin. It checks that a failed attempt preserves shown/queued
views and media, while successful Begin revokes them before any expiry sweep.
Continuation refusal keeps the exact journal/header cursor. Garbage cleanup
refusal keeps both the Complete blob row and owned bytes; the accepted path
checks both native Execute commits surrounding owned-file deletion. The views
fixture separately holds the production service delivery across a purge.
These are written headless cases until their recorded build/run qualification.

### Independent History revocation

A History box owns a separate account/store/chat-generation capability, even
when opened from Deleted messages. Closing its parent does not close History.
Execute checks registered History capabilities in the same worker step as
deleted snapshots and media. A purge/admission or generation change revokes
queued results and shown actions, and posts a main-queue notification to clear
and close the History box without another read. Ordinary writes retain the
capability; pagination still checks its own revision. Up to 64 independent
History capabilities are admitted, with inactive entries removed before reuse.
Logout/discard/close revoke them before database closure. No UI database read
is introduced.

### Confirmation scope and expected reader retirement

Queued confirmation permission listens to native realtime migration updates.
An alias change latches permission off on the main thread before the worker can
Begin; the worker never calls the peer-reading UI closure. A fresh confirmation
is required even if the archive revision did not change.

Finish bulk lease recovery suppresses reentrant media-map refresh until its
cached lease page has been drained, then retires native handles through positive
absence proofs. Healthy Finish clears the worker claim and renews capture without
Retry. A real IO or unconfirmed retirement remains visibly held.

Owned handles revoked by a confirmed maintenance operation may acknowledge a
release as deferred while its pins are kept. After confirmed release or exact
absence, bounded weak retirement records accept only the same full capability
for a late callback. Purposeful retirement produces no retention-gap warning;
unknown handles, forged bindings and actual release failures still refuse. At
most 64 active-plus-live-retired capabilities are retained, and expired weak
records are pruned before another Open. Close/logout drops this memory state.

D4b passive streaming admission is an owned, memory-only capability for the
opening journal and store/login lifetime. Execute inspects the authenticated
post-statement header before COMMIT and revokes the captured capability when
the committed transaction closes normal admission or changes the account
generation. This happens before Execute returns or publishes purge effects.
Ordinary compatible writes retain admission. A later journal gets a new
capability; it cannot reactivate a retained old sink. Logout, discard and close
revoke the capability even if subsequent cleanup fails. GUI callbacks perform
no fence reads and refuse revoked producers before posting any work.

D4b distinguishes an authenticated maintenance gate from an unreadable or
unauthenticated admission header. A committed write with an unreadable header
revokes the old GUI capability and reports capture-admission-header-unreadable.
The worker makes at most three renewal attempts, five seconds apart after the
first immediate attempt, including journal open/drain failures in that budget.
Each attempt validates the same store/login epoch, account generation and all
bounded authenticated chat generations. Only unchanged scope may publish a new
journal capability; old producers stay revoked. Changed scope and exhausted
retries remain visibly held. A successful replacement reports
capture-admission-renewed. Close, discard and logout cancel recovery and revoke
all candidates. Healthy purges still revoke at the Execute committed boundary.

Journal replacement retires the previous writer on the account worker before
opening a new sequence allocator. Retirement permanently refuses new Appends
and waits at most 50 ms for an already-entered Append to finish or fail. The
fresh journal then loads the sequence actually saved by the old writer. A
retirement timeout consumes one of the same three recovery attempts and leaves
a visible held reason; no second allocator opens. Queued old-writer callbacks
leave replay to the replacement instead of draining with a stale sequence.
The GUI only pauses admission and never waits for retirement. Stop/logout and
discard cancel replacement immediately, including during the bounded wait.

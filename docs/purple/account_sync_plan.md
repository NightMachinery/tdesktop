# Account-backed Purple sync proposal

Status: design plus shared-core and client-local foundations. Desktop has a
read-only **Sync across devices** setup box that can inspect one explicitly
selected account's Saved Messages. Account-backed sync is not enabled in either
client; the existing manual Send/Import actions remain the current transfer
behavior.

The shared core now has tested config version construction, remote-head
classification, strict JSON canonicalization, validated uncompressed record
envelopes, config payload inspection and construction, and a versioned
device-local sync state. It also derives sync status, attention tier, and the
primary recovery action from engine facts in one tested model. Clients still
need to supply those facts, localize the labels, and deduplicate notices; the
model does not activate account sync. The shared builder accepts exact UTF-8
settings bytes, full parent versions, writer metadata, and a sequence. It
emits a canonical record only after parsing and inspecting its own output. A
newer settings schema remains read-only in an older client.

When an install applies another install's config record, it can acknowledge
that exact version under its own writer identity and sequence. The builder
preserves the remote key, parents, and lineage, and rejects a text mismatch or
invalid ancestry. This lets other installs classify the acknowledgement as
the same config instead of mistaking it for a new edit.

The state records each stream's issued, pending, and confirmed sequence, its
last payload hash, and the config version ancestry. Core's read-back check
compares sequence and payload hash; a higher own sequence or a matching
sequence with a different hash signals a clone or rewind. The client bridges
also require the full server record to match the staged canonical bytes before
confirming it. Shared core has an optional ledger of confirmed own message IDs,
bounded to 256 entries, with a hash of each entire canonical record. It permits
cleanup only for an older sequence after a fresh exact server read-back. A
current confirmed record stays protected even when a duplicate was posted.
Each stream also has a bounded log of the last 32 issued sequence and full
record-hash pairs. Desktop staging writes the new pair with the reserved state
before sending. A later discovered duplicate, including one from a previous
sync space, can enter the ledger only when its complete record matches an
issued pair and the install and creating device match. An unlogged old record
cannot be retired. Android's JNI bridge returns the issued pair together with
the reserved config state; neither app calls an account-backed publisher yet.
Shared core now also has an uncalled pure publish planner for one stream. Given
binding, completed discovery, own-record observations, a staged record, and
per-stream policy, it chooses wait, reconcile, stage, edit, post, or an older
ledger message to inspect for retirement. It never performs Telegram I/O or
authorizes deletion by itself; the caller must re-read the exact server record
and run the existing deletion check. A library publish remains gated on a
separate validated library payload and future library ledger operations.
Neither client enables account-backed sync yet. Desktop-local storage can
acquire an exclusive `sync/lock`, reject malformed or newer state, and stage
canonical config bytes
before committing a reserved `sync/state.json`. It creates no state while sync
is off, keeps `sync/` and its pending files owner-only on Unix, and pauses when
a pending file is absent or disagrees with the state. The desktop local store
also refuses to stage a config record unless the selected account's binding
token matches the token in that state. The per-account preference write is
delayed by Telegram's local storage layer, so a crash between the state and
preference writes leaves sync unbound and unable to publish.
Desktop account logout clears the account's cached preferences and cancels
their pending write, so a new login in that slot cannot inherit the old token.
The store accepts a validated own config read-back with its Telegram message
ID. It confirms only the exact staged record and atomically records that ID
with the confirmed state and staged version as base with its lineage.
A changed content
fingerprint clears old equivalent keys;
`seen_seq` survives. It writes confirmed state before deleting the staged
envelope. After a crash in that gap, restart deletes only a validated stage
matching the confirmed state. Older staged own records are also obsolete once
a later sequence is persisted, even when a user chooses an unrelated remote
version; restart validates their canonical contents, writer and sequence before
removal. Ambiguous and future files pause cleanup. A clone or device mismatch,
including a missing current device identity, has a distinct verdict.
The desktop store latches that verdict and refuses further staging until a
fresh open and reconciliation. `QSaveFile` protects final files from partial
writes after a process crash; it does not promise persistence through power
loss. Desktop also has an uncalled Saved Messages history scanner. It pages
`messages.getHistory` to an empty result, reports progress, and only returns
candidate message IDs after a complete scan. Cancellation, request failure,
invalid responses, and stalled pagination remain incomplete. A separate
uncalled reader re-reads each candidate by ID, checks that it is still an
original document in Saved Messages, caps its download at 4 MiB, and classifies
the record envelope and config payload. Missing, changed, oversized, newer,
invalid, or inaccessible candidates stay visible as unresolved results; they
cannot establish that the account has no sync group. An uncalled inventory
coordinator now joins the two operations for one explicitly selected account:
it returns Complete only after both the full scan and every candidate read
complete. An incomplete scan never starts candidate reading. The reader can
populate Telegram's local download cache, but sends and deletes nothing.
It retains the document ID and edit date alongside each re-read candidate for
later changed-media checks and opaque-header caching.
The reader preserves validated space and writer headers for records from future
streams or encodings while keeping their payload opaque. Such headers count as
existing sync records rather than unreadable candidates; the setup box reports
how many this version cannot read. Identity-encoded library envelopes are also
header-only until a playlist payload validator exists. Newer-major and invalid
candidates still require review before the client can make a group decision.

The read-only desktop inventory now resolves completed candidate reads into
per-space, per-stream, per-install heads. Equal-sequence conflicting records
block safe space selection; opaque future and library headers keep their space
visible without becoming supported heads. It still never publishes.

An uncalled desktop config post adapter can now take an explicitly selected
account and a staged canonical record, send one JSON document to that account's
Saved Messages, then re-read the returned message ID. It reports confirmation
only when the server document matches the staged bytes exactly. A missing send
receipt or failed read-back remains uncertain and must be reconciled against
history before any retry. A changed or mismatched document needs review. The
adapter does not reserve local state, retry, enable sync, or post on its own.

The desktop setup box can run this inventory against one signed-in account and
shows scan progress and the resulting complete, needs-review, or incomplete
state. With multiple accounts it requires an explicit choice. It does not yet
bind an account, create a sync space, or enable transport. The future transport
engine must reconcile the install's own remote record for clone or rewind
signals before any publish. Android's uncalled bridge can initialize canonical
local state bound to the active Telegram user in its account slot, reserve a
canonical own config record with its pending version key, and confirm an exact
staged server read-back. It can record and check own message IDs for cleanup.
It validates the record's space, install and device identity. The binding token
is saved synchronously in a per-user account preference; reservation refuses a
missing or mismatched token. An uncalled Android store now persists canonical
state and exact staged config bytes under app-private `purple/sync`, holding an
exclusive lock and pausing on ambiguous or mismatched recovery files. It
confirms only an exact server read-back before clearing a validated stage.
Android network transport and the account-sync interface remain to be built.

The read-only scanner and candidate reader now leave Telegram's standard
`FLOOD_WAIT` retry enabled. A rate-limit wait no longer immediately turns the
inventory incomplete, but the setup box does not yet show a countdown or
resume a scan after an app restart. Those are required before automatic sync.

Compressed library records remain unsupported until the playlist phase.
A deterministic shared-core simulation now exercises three devices against a
fake Saved Messages store, including delayed search, failed upload, edit and
repost modes, conflict classification, and restart from staged publish bytes.

Purple can use each account's Saved Messages to carry configuration and future
playlists between that account's devices. This needs no extra service or
credentials. Saved Messages is a Telegram cloud chat, not an end-to-end
encrypted store (see Telegram's
[FAQ](https://telegram.org/faq#q-so-how-do-you-encrypt-data)). The sync screen
must explain that settings and playlist metadata become available to Telegram's
cloud and to every session signed in to the account. Device-local `state.toml`
stays local.

One record layer should serve both streams: account binding, space discovery,
record validation, publish and read-back confirmation, duplicate reconciliation,
and safe retirement. Each install owns one current record per stream. The
`config` payload remains a whole-file version that asks before applying;
the future `library` payload should be a full mergeable playlist replica.
Neither audio files nor download/cache choices belong in the library record.
Before enabling transport, older clients must recognize valid records for
unknown streams as evidence of an existing space, and account binding must
survive a space move without silently changing accounts. Lost send responses
also need reconciliation before retry, with a bounded issued-record history so
late duplicate posts can be retired safely.

A complete Saved Messages history scan remains required at initial setup.
Incremental `messages.getHistory` above a lagged message-ID watermark may
replace routine full rescans if disposable-account tests confirm its
completeness; known record IDs still need explicit re-reads for edits and
deletions. Until then, the daily complete recovery scan below remains the
conservative plan. The scanner must wait through `FLOOD_WAIT` and resume
without treating an incomplete scan as an empty account.

Telegram documents that `messages.getHistory` returns descending-date history
and applies `min_id` after the offset-and-limit slice (see
[getHistory](https://core.telegram.org/method/messages.getHistory) and
[pagination](https://core.telegram.org/api/offsets)). The disposable-account
watermark test must cover messages arriving during pagination and filtered
short pages; a short page alone must never mean that a pass is complete.

## Record transport

Each install receives a random 128-bit identity and chooses one home account.
Setup also generates a separate random 128-bit binding token. It stores one
copy in the device-local sync state and one in that Telegram account's
local preferences. Desktop encrypts its account preferences. Android uses a
preference key scoped to the active Telegram user ID because its preference
file is scoped to an account slot and can survive logout. A publish is allowed
only when both copies are present and equal for the selected account. Neither
a sync-space move nor an install-ID regeneration changes the binding token.
Copying only the Purple config directory to another machine leaves sync
unbound instead of silently publishing through whichever Telegram account is
signed in there.
Missing or mismatched copies pause sync until the user explicitly binds an
account again; the state file does not need to record a Telegram user ID.
Each platform supplies 16 secure random bytes for an install ID. For a new
sync-space ID it supplies a server-time estimate in milliseconds and 10 secure
random bytes; shared core formats the same lowercase base32 IDs on both
platforms and compares decoded space IDs in immutable creation order. Android
exposes these helpers through JNI, but no runtime setup path calls them yet.
An install publishes its own versioned JSON document for each stream: `config` for
`settings.toml`, and later `library` for playlists. No other install edits that
record. First setup pages through the chosen account's complete Saved Messages
history before deciding that no sync group exists. A hashtag search alone
cannot prove absence: a delayed search entry may later appear below an
advanced message-ID watermark. Later checks re-read known IDs, use a hashtag
search for quick discovery, and repeat the full history scan on Sync now and
daily for recovery. Incomplete scans never authorize a new group. Readers
validate candidate documents and reconcile them locally. This
one-writer rule avoids losing updates through Telegram's message-edit API, which
offers no compare-and-swap.

The payload hash uses [RFC 8785 JSON canonicalization](https://www.rfc-editor.org/rfc/rfc8785.html).
Version 1 records reject duplicate object keys and invalid Unicode. Every JSON
number must be an exact safe integer; large Telegram identifiers are strings.
Unknown fields must follow the same rule. This gives both clients the same hash
even if their JSON writers order properties differently.

The preferred transport replaces each install's document in place, keeping Saved
Messages uncluttered. Telegram's
[edit API](https://core.telegram.org/method/messages.editMessage) accepts
replacement media, and source inspection suggests that both clients can edit
document media in the self chat, but this has not been verified between clients.
Before implementation, use a disposable account to test replacement in both
directions, after reconnect, and after repeated edits. If it fails, publish a
replacement message and retire the install's superseded record. The record
format and sync UI can be the same in either mode.

An Android-only disposable-account probe on 2026-09-29 sent an inert 20 KiB
document to Saved Messages, then used the message's **Edit** and **Replace
file** actions to substitute another inert document. The chat displayed one
probe message with the new filename, an edited label, and a sent checkmark.
After force-stopping and reopening the app, the same result remained visible.
This verifies the Android UI path and app-restart display, but not the server
message ID, a fresh API read-back, or visibility from another client. The probe
message is retained in the disposable account for the old-message edit test.
Edit-in-place therefore remains disabled as a sync transport until the
cross-client, read-back, age-limit, and repeated-edit probes pass.

## Proposed user flow

Each client will show one **Sync across devices** entry in Purple settings.
Turning it on binds that device to one signed-in Telegram account and checks
that account's Saved Messages before sending anything. On a device with several
accounts, setup should require an explicit account choice and explain that the
single installation-wide settings file may include chat names and identifiers
from the other accounts. This costs one extra tap but prevents an accidental
cross-account upload to a preselected account. Setup waits
for a successful check; offline setup must not silently create a separate
sync group. Sync remains opt-in on every device.

Incoming settings stay in **Update ready** until reviewed. One notice per
version opens a short change preview with Apply or Not now. Applying keeps the
replaced file in local History and offers Undo. Different edits on two devices
require an explicit whole-file choice; the rejected version stays in History.
Routine checks and uploads are silent. The settings row always shows the
current state, while a persistent chat-list card is reserved for a conflict or
a problem that pauses sync. Pause, turn off, Sync now, History and device
details live on the same screen. Turning sync off keeps the local settings and
restores the legacy Send/Import behavior.

Every install owns only its own sync messages. The device-local ledger ties
each confirmed message ID to the exact sequence and full record it posted;
replacing or deleting an old message requires a matching server read-back.
The transport still needs to perform those reads and deletes. If the user's
own sync message disappears or changes unexpectedly, the client pauses and
asks instead of recreating or deleting it. Legacy manual posts and explicit
**Save a copy** snapshots are never touched. A rare fork in which two devices
start separate sync groups must use a stable, immutable ordering of group IDs.
Moving a device to the selected group without changing its settings may happen
silently; different settings require a choice and remain recoverable. The
group-order encoding is tested in shared core; fork migration cases still need
tests before transport integration.

## Settings first

Start with opt-in, per-device sync in Ask mode. App-made settings edits publish
automatically. The proposed UX also publishes valid desktop text-editor saves
after a quiet period, with the receiving device still asking before Apply. A
receiving device retains a visible pending update until the user resolves or
snoozes it. Each whole-file version records its ancestry, so a device can
distinguish a newer version from concurrent edits. Concurrent versions require
an explicit choice, and the discarded side remains in local History with Undo.
There is no automatic TOML merge in this first release.

Before this new flow, repair the current manual sharing path: prevent self-echo
offers, mark a send complete only after server confirmation, align import limits
between clients, and preserve Android's pre-import backup across editor saves.
Update the existing documentation to describe Saved Messages' cloud privacy
accurately.

Android source `6df36b1d` now keeps a separate pre-import backup across editor
saves, and `a950e13c` matches desktop's 4 MiB import and 100-document search
limits. That signed build opened the retained test account on 2026-09-29; the
backup and large-file boundaries have not yet been exercised in a live import.
Both legacy sharing paths now wait for a confirmed server message id before
recording the sent fingerprint and advancing the account's offer watermark.
An older send that finishes after a newer edit cannot replace the current
file's sent fingerprint. The Android receipt patch built from exact source
`8b07614c`, passed signature checks, and upgraded the preserved emulator
without losing its login; its signed APK was delivered privately. Neither
client has had a live receipt test; compile and startup checks do not prove
server acceptance handling.

On desktop, `UploadTo` returns after `sendFiles` queues file preparation. A
single-file receipt follows the post through preparation and upload to the
`sendPreparedMessage` server response. Only a response with this post's server
message id records the fingerprint and advances the import-offer watermark.
Preparation, upload, and request failures leave both unchanged. A fingerprint
pending in memory prevents a second local write from queuing duplicate bytes
while the first is in flight. Telegram's
[update protocol](https://core.telegram.org/api/updates) says `random_id`
deduplicates sends across an account's sessions and `updateMessageID` can later
identify a message whose response was lost. The current legacy path does not
reconcile a lost response after restart. A later explicit save or manual send
may create a duplicate if the server accepted the first post; durable
`random_id` reconciliation belongs in the account-backed design. This pipeline
has not yet been tested live.

On Android, a Purple-only document receipt follows the asynchronous send
helper to the server result. Each attempt stages an immutable file with the
required `settings.toml` name, and concurrent sends of identical bytes on one
account share the pending attempt. Telegram's own Retry action after a failed
send does not preserve this Purple receipt, so a successful manual retry can
still cause a later duplicate or self-offer. The new sync engine needs durable
reconciliation rather than extending this legacy callback indefinitely.

## Playlists later

Playlists will use the same account binding, record envelope, discovery, and
transport. Their data model should be revisited with the playlist feature.
Each install's library record should contain its full merged replica, not just
that install's edits. A later device can then recover the library from any
surviving replica. Before removing a stale device's record, another device
must absorb its exact content and confirm that the merged state is present in
its own record.

Unlike a whole settings file, playlist entries can be edited concurrently on
different devices. The proposed library uses stable song and entry identities,
per-field deterministic resolution, ordering, and deletion markers. Edits to
different fields combine; a delete wins over concurrent edits but remains
restorable. A merge must be commutative, associative, and idempotent, and must
never create a new edit stamp. A local overwrite log should retain values
replaced by a merge, because a short ring of whole-library snapshots could
discard them after a busy day of syncing. The exact stamp and tombstone format
remains part of the playlist design and its convergence tests.

Only a changed library payload hash warrants a new record. In edit-in-place
mode, a merge-only republish can be rate-limited; in append-and-retire mode it
should normally wait for a local edit or a daily flush, so another device's
playlist edit does not repeatedly bump Saved Messages. Future library fields
must survive an older client's merge, and a format with a newer minimum writer
version must make that client read-only.

The library stores message and document identities, not audio files,
account-specific access hashes, or file references. A song in Saved Messages
uses a `self` peer token instead of the account's numeric user ID. Unavailable
songs remain visible with an explanation instead of disappearing. Download
state, playback state, and any **Keep downloaded** preference stay device-local.

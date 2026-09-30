# Account-backed Purple sync proposal

Status: design plus shared-core foundations and a manual desktop settings
sync. Desktop's **Sync across devices** box checks one explicitly selected
account's Saved Messages, reviews the settings records there, and offers one
next step: publish the first record, join a device with the same file, review
and apply an update, choose between diverged versions, publish local changes,
or finish an earlier post. Every step runs only when clicked, and every post
follows a fresh check and a confirmation with the cloud disclosure. Replaced
files go to a local History with Restore and Undo. Android has the same
manual flow under Settings → Purple → **Sync across devices**, decided by the
same purple-core code. Background checks, automatic publish, edit in place,
retirement of superseded records, and live verification, including a
cross-client test between desktop and Android, remain. The existing manual
Send/Import actions remain available.

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
the reserved config state, and Android's manual publisher stages through it.
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
loss. Desktop also has a Saved Messages history scanner. It pages
`messages.getHistory` to an empty result, reports progress, and only returns
candidate message IDs after a complete scan. Cancellation, request failure,
invalid responses, and stalled pagination remain incomplete. A separate
reader re-reads each candidate by ID, checks that it is still an
original document in Saved Messages, caps its download at 4 MiB, and classifies
the record envelope and config payload. Missing, changed, oversized, newer,
invalid, or inaccessible candidates stay visible as unresolved results; they
cannot establish that the account has no sync group. An inventory
coordinator joins the two operations for one explicitly selected account:
it returns Complete only after both the full scan and every candidate read
complete. An incomplete scan never starts candidate reading. The reader can
populate Telegram's local download cache, but sends and deletes nothing.
It retains the document ID and edit date alongside each re-read candidate for
later changed-media checks and opaque-header caching.
The reader preserves validated space and writer headers for records from future
streams or encodings while keeping their payload opaque. Such headers count as
existing sync records rather than unreadable candidates, so an account that
holds only such records is never treated as empty; the settings review ignores
streams other than `config`. Identity-encoded library envelopes are also
header-only until a playlist payload validator exists. Newer-major and invalid
candidates still require review before the client can make a group decision.
The rules in this paragraph (the candidate test, the page rule, record
classification, the read and inventory statuses, the directory candidates)
live in purple-core's `purple_sync_inventory` so the Android client decides
them identically; desktop keeps only the Telegram requests, the download and
the progress and cancel handling.

The read-only desktop inventory now resolves completed candidate reads into
per-space, per-stream, per-install heads. Equal-sequence conflicting records
block safe space selection; opaque future and library headers keep their space
visible without becoming supported heads. It still never publishes.

Sync records never land in a user folder on desktop. Telegram Desktop
normally saves a copy of each file it sends from memory into the download
folder (`Uploader::upload`), and its automatic download saves a file that
comes into view in a chat or in Shared Files there too
(`DocumentMedia::automaticLoad`). Two hooks in upstream files stop both for
records. `DocumentData::saveToCache()` is true for a document named exactly
`Purple settings sync.json` or `Purple playlists sync.json`
(`Purple::IsSyncRecordFileName`, built from purple-core's
`SyncSettingsRecordFileName` and `SyncPlaylistsRecordFileName`), as it is for
stickers: the bytes go to the profile's encrypted media cache, automatic
download fetches the record into that cache, and the upload writes no copy.
`Uploader::upload` also skips the copy for every Purple post
(`Purple::IsPurplePost`: a send carrying the receipt that only Purple's sends
ask for), which covers the legacy `settings.toml` sender as well. Automatic
download of legacy `settings.toml` documents stays as upstream, as on Android.
Opening a record or saving it by hand still writes a file to the download
folder, because the person asked for one. `purple/test_sync_record_names.sh`
checks the name rule, with near misses, against core's candidate rule. Android
differs in one way: it does not auto-download records at all
(`PurpleSyncAutoDownload`), while desktop auto-downloads them into the
encrypted cache. Copies that earlier builds wrote to the download folder stay
there until deleted by hand.

An uncalled desktop config post adapter can now take an explicitly selected
account and a staged canonical record, send one JSON document to that account's
Saved Messages, then re-read the returned message ID. It reports confirmation
only when the server document matches the staged bytes exactly. A missing send
receipt or failed read-back remains uncertain and must be reconciled against
history before any retry. A changed or mismatched document needs review. The
adapter does not reserve local state, retry, enable sync, or post on its own.
An own-config inventory matcher (`ReconcileOwnConfigInventory`, also in
purple-core's `purple_sync_inventory`) checks a completed scan against the
install's local state. It rejects a different account, an unsafe space,
wrong device identity, noncanonical own records, divergent records at one
sequence, and a staged record whose full hash is absent from the issued log.
It reports the highest own head and any exact pending record already found in
Saved Messages. The future publisher must use that pending message for exact
confirmation instead of posting again; an absent result alone does not cause
a retry.

The desktop one-shot config publisher composes the local store,
own-record matcher, pure publish planner, and exact post/read-back adapter.
For an empty config space it can reserve and stage the current settings record
before one post, then confirm only the returned exact server bytes. On restart
it can instead confirm a staged record already found in a fresh complete scan;
when that scan finds no staged record, the planner still decides whether one
retry is safe. Uncertain sends leave the stage in place. With nothing staged,
the publisher now plans on the click inventory with the shared planner, using
the stored config state, the other installs' newest records and this install's
own newest record. An empty space publishes a first record with no parents,
exactly as before. Local changes publish with the base as a parent, plus this
install's own record when it no longer describes the settings. Choose and
Conflict publish only when the caller passes the parent keys the apply step
reported, and the planned parents must match them as a set; a caller without
them cannot publish over another device's version. Up to date reports already
synced. An update, a silent adoption, a pending send, an invalid state, or a
choice that would first adopt same-content heads stops for review, because the
apply step must run first. The record carries those parents and the next
sequence and is written by the platform it runs on (Windows, macOS or Linux),
is staged with its version key as pending, and is posted and confirmed as
before. It does not yet retire superseded messages. Every start carries a
request, and the publisher enforces its mode before anything else. A
pending-only request confirms or posts the existing stage and refuses when
nothing is staged, and it may not carry expectations. Any other request posts
new content only when it carries both the reviewed settings fingerprint and
the expected parent keys, the file still has that fingerprint, the planned
parents match as a set, and no record is staged; a stage found by such a
request stops for review. The setup box uses the first mode for Finish sending
and the second for Publish settings, Publish changes and the share half of Use
and share, with the fingerprint and parents from the apply step. A request
without expectations, or a pending-only start that finds another window
already finished the post, therefore cannot publish edits the user never
reviewed. There is no standing resume action. This does not enable continuous
account-backed sync. The decisions in this paragraph (the request gate, the own
record reconcile, the gate plan, building the record and both publish planner
calls) live in purple-core's `purple_sync_config_flow` as `PlanSyncConfigPost`
and `PlanSyncConfigStagedPost`, so Android makes the same ones; the desktop
publisher keeps the store, the account checks and the post.

Finish sending refuses to post while an earlier copy is still queued.
Cancelling a publish, closing the box or losing the receipt only stops the
publisher from listening: the upload and `messages.sendMedia` stay in the
session's send queue and go out when the connection or upload queue allows,
for as long as the app runs. A check in the meantime reads Saved Messages from
the server, cannot see the queued message, still finds the stage and offers
Finish sending. So desktop asks the session's data layer whether Saved
Messages holds a local item the server has not confirmed yet, still sending or
marked failed, whose document is named `Purple settings sync.json`
(`SyncConfigSendQueueOf`, which walks `History::clientSideMessages()` of the
self chat). It asks twice: when the click's check starts scanning Saved
Messages, just before the first history page is requested, and again when the
publisher plans the post. Core's `PlanSyncConfigPost` receives HoldsSyncRecord
if either answer held a copy, and then returns StillSending instead of posting
the staged bytes again.

The first read closes a race that Android acceptance hit live. A copy that
Telegram delivers during the scan lands above history pages that were already
read, so the inventory lacks it, and it has left the queue by the time the post
is planned. A single late read then sees neither the record nor the queued
copy, and Finish sending posts a byte-equal duplicate. On desktop the window is
wider than on Android, because the confirmation dialog sits between the check
and the plan. The read at the scan start sees the copy while it is still
queued; the read at planning catches anything queued after that.

The box says that an earlier copy is still being sent, or failed, in
Saved Messages, and asks the person to wait until it arrives and check again,
or to delete the failed copy there first. Nothing retries on its own and no
timer runs. When the earlier copy arrives, the next check finds it and Finish
sending confirms it without posting; that confirmation wins even while the
queue still lists a copy.

The list is the session's `History` object for Saved Messages, not a widget, so
it holds items queued by earlier setup boxes or from the chat itself for the
whole session. An item leaves it only when the server assigns its real id or
the item is destroyed. A failed copy therefore keeps Finish sending refused
until the person deletes it, which clears the refusal, or resends it from the
chat, which ends like any other arrival. A file of that name sent by hand is
refused the same way, which only delays the post.

Two narrow gaps remain. `sendFiles` prepares the document on the file loader
queue before `Api::SendConfirmedFile` creates the local item, so during that
preparation, normally milliseconds for a record under 256 KiB but longer when
large files wait ahead of it in the same queue, the post is in flight and not
yet in the list. Quitting the app still closes the window: tdesktop does not
keep unsent media sends across a restart, so a copy either reached the server
before the quit, where the next check finds and confirms it, or is gone, and
Finish sending then posts it once. Both points are read from the code, not
observed live. If two copies arrive anyway, Saved Messages holds two
byte-identical records at one sequence; the own-record reconcile lists the
later one as a duplicate and every decision stays the same, so the cost is a
redundant message that every later check downloads. Android applies the same
core rule; its queue query reads Telegram's local message table for unsent
and failed copies of the record, which on Android survive a restart.

The desktop setup box runs this inventory against one signed-in account,
shows scan progress, and passes a complete result to the review step described
below. With multiple accounts it requires an explicit choice. Each inventory
result carries the account's numeric user ID, so setup cannot use one
account's scan to bind another. The box keeps only its latest review for the
same live account and session, and discards it on a new check, cancel, account
switch, logout, or box close; a generation counter drops every confirmation,
review choice, or apply result that belongs to an older check. The review's
verdict picks one action. Empty offers Publish settings. Adopt offers Join sync
on an unlinked device; on a linked one the check adopts silently and reviews
again. UpdateReady offers Review update, Choose and Conflict offer Choose
settings, LocalChanges offers Publish changes, and Pending offers Finish
sending, but only from a ready review, so an Invalid verdict or unusable
foreign records pause a staged post as the planner's precedence requires; the
box then only explains the pause. Up to date reports the number of other
devices and the check time. Invalid verdicts and every failure are plain text
with no action. Publish settings, Publish changes
and Finish sending start a new full scan on the click, review it again, and
open the confirmation only when that review still shows the same action;
otherwise the box reports that nothing was sent. Join sync, Apply and a
choice act on the Saved Messages snapshot of the check they came from; the
apply step re-reads the file and the local state but not Saved Messages, which
is accepted because none of them posts, and every post scans again. Review
update and Choose settings open a review box with the source device and time,
a summary of up to six changed settings, a newer-schema warning, and a Show
lines diff. A choice that also publishes scans again before the post. While
a scan or post runs, the action, Undo and History restore are disabled, since
those hold the local sync store. Devices are named by the record's platform
and the first four characters of the install ID after the `in-` prefix.
Which sentence and action a review deserves, the choices the review box offers
and how a failed apply is worded come from purple-core's
`purple_sync_config_describe`; desktop keeps only the English text.
Android's Sync across devices screen follows the same verdict-to-action rules
and the same English, kept in `strings.xml`. It has no account list: it checks
the account the Purple settings screen belongs to and names it at the top, so
syncing another account means switching to it in Telegram first. Its update
notice is a bulletin with an Undo button, beside the same Undo last update
row, and closing the screen cancels a check as closing the box does.
The local setup operation creates an install identity and account binding only
after a complete, unambiguous scan. It reuses the selected existing space or
creates a time-ordered space ID when the account has none. An existing local
state is never silently rebound or moved. A binding preference that has not
persisted at a crash leaves the state unbound and unable to publish. Every
confirmation that links or posts explains that settings may include chat IDs
and names and that Saved Messages is a Telegram cloud chat readable by other
signed-in sessions, including Join sync, which sends nothing. Results report
confirmation, uncertainty, review, binding, and store failures without an
automatic retry. An uncertain outcome requires a new check before anything
else. Joining is saved before History and the file, so a later failure in the
same apply reports "Joined sync, but ..." and that nothing was written to
`settings.toml`; the next check sees a linked device with an empty base and
offers Choose or Adopt again. Undo last update is offered after any apply that
wrote the file, including one whose state commit failed, unless the saved copy
records a missing file or is not valid UTF-8 text. A failed Undo keeps the row
unless the copy is gone or can never be restored. The future
transport engine must reconcile the install's own remote record for clone or
rewind signals before any publish. Android's bridge can initialize
canonical local state bound to the active Telegram user in its account slot,
reserve a canonical own config record with its pending version key, and
confirm an exact staged server read-back. It can record and check own message
IDs for cleanup.
It validates the record's space, install and device identity. The binding token
is saved synchronously in a per-user account preference; reservation refuses a
missing or mismatched token. An Android store persists canonical state and
exact staged config bytes under app-private `purple/sync`, holding an
exclusive lock and pausing on ambiguous or mismatched recovery files. It
confirms only an exact server read-back before clearing a validated stage.
Android's Telegram transport, its join, apply and publish executors and the
Sync across devices screen drive these for one account at a time; the Android
repository's `docs/account-sync-store.md` and `docs/account-sync-transport.md`
describe them.

Desktop has a settings History store for the update, choice, restore, and
undo actions. Each entry keeps the exact `settings.toml`
bytes in `sync/history/` under the Purple config directory, next to a metadata
file with the creation time, the reason, a short label, the config version key
when known, and the settings fingerprint of those bytes. The metadata also
records whether the settings file existed, so a later restore can recreate a
missing file. Entries are capped at 256 KiB, the same limit a settings record
has. Entry IDs begin with the creation time in milliseconds and end with a
random suffix. The directories and files are owner-only on Unix, and an
insecure directory refuses every operation, as in the local sync store. A
reader accepts an entry only when both files exist, the metadata parses, and
the bytes match the recorded fingerprint. After a new entry is fully written,
the store keeps the newest 30 valid entries, deletes older ones and invalid
leftovers older than those 30, and never deletes anything outside that
directory. A save can name one more entry to keep for that pruning pass;
Restore and Undo name their target, so pruning never removes it before its
bytes are written back. Only the apply and restore steps below write to it, and
the manual import still keeps its single `settings.toml.bak`.

The desktop local store can also commit new config data without staging a
record. A later update or join step needs this to record an adopted remote
version, equivalent keys, or newer `seen_seq` values without posting anything.
It works only on an opened, ready store and only with the selected account's
binding token. It refuses while a config record is staged, when the new data
carries a pending key, and when any `seen_seq` entry would decrease or
disappear. The whole next state must pass the shared serializer and parser,
and it is saved with the same atomic state write that staging uses. Committing
unchanged data writes nothing. Only the apply step below uses it. The checks
are purple-core's `CheckSyncConfigDataCommit`; the store only writes the
canonical bytes it returns.

Desktop has a review step for an existing sync space. From a
complete inventory it extracts, for every other install in the selected space,
that install's newest supported config record: the version key and lineage,
the exact settings text, the writer's device, platform and app, the send time,
and the Telegram message ID. A group without a supported head, or a head
record that does not parse or does not match the record hash the directory
saw, makes the review stop for review instead of being dropped. The review
also finds this install's own newest config record through the own-record
matcher and reads `settings.toml`. A missing file is allowed and fingerprints
as empty bytes; a symlink, another non-regular file or a file over 256 KiB is
refused, as in the publisher. Then it asks the shared planner for a verdict.
A bound install plans with its stored config state and its own newest record,
so the planner can tell when this device's cloud record no longer describes
its settings; without that record two devices that picked each other's
versions could both report up to date with different files. An unbound install
previews the same planner with an empty install in the selected space, and an
account with no sync records at all is Empty. A bound state whose space
differs from the inventory's selected space stops for review, because moving
spaces is not supported. A clone, an unresolved own record or an incomplete
inventory stops the same way. The step holds the sync store lock only while it
runs and writes nothing. The same file holds a publish gate that plans on the
click inventory in the same way; the publisher uses it for every post that
is not already staged. The review, head extraction and gate live in
purple-core's `purple_sync_config_flow` (`ReviewSyncConfigInventory`,
`ExtractSyncConfigHeads`, `PlanSyncConfigPublishGate`); desktop reads the file,
opens the store and checks the account.

While `settings.toml` is missing or does not load, the app runs from
`settings.toml.good`, its last accepted copy (see [config.md](config.md)). A
sync write in that state would lose the settings the device is running:
History would keep only the broken or missing file, and the reload after the
write would overwrite `settings.toml.good` with the synced version. So
`ReadSyncSettingsFile`, which every sync read of the file goes through (the
review, both fresh reviews of an apply, and the publisher), passes
`UsingLastGoodSettings()` as core's `usingLastGood` flag, and core refuses the
review with UsingLastGood. The box says that the file is missing or does not
load, that this device runs its last working copy, and that sync changes
nothing until the file is fixed or restored; it offers no action. An apply of
a review shown before the fallback stops with NeedsRecheck, and a new-content
post stops with InvalidSettings. Restore and Undo are not sync and stay
available, since restoring a version is one way out; once the restored file
loads, the reload replaces `settings.toml.good` with it, so the copy the device
was running is not kept.

An apply step performs the local half of a choice made in that
review, and never posts. It repeats the account checks, then reviews again and
compares review stamps (purple-core's `SyncConfigReviewStamp`): a digest of the
review status, the account, whether the device is linked, the space and
install, the file's status and fingerprint, the verdict, the own-record
staleness, the offered and same-content keys, every head's message ID, space,
install, sequence, key, lineage and text hash, and the own record's message ID
and identity. If the stamp of the fresh review differs from the reviewed one,
it stops and asks for a new check. An unbound install compares an unlinked
review of the inventory it is given and a fresh read of `settings.toml`, then
joins, creating its install identity and binding in the selected space, or in
a new space when the account has none. With the store open it reviews the
inventory again with the stored state, the staged record and another fresh
read of the file, and compares that stamp with the reviewed one (for a device
that just joined, with the reviewed one as it looks once linked). What to
write, the History reason and version key, the adoption and the next-publish
proposal come from purple-core's `PlanSyncConfigApply` and
`CompleteSyncConfigApply`; desktop does the History save, the write, the read
back and the commit.
Writing another device's version first saves the current file to History,
with the reason before update or before choice, a label such as "Before
update from Android 9c1d", and the local version key when the file still
matches the base. A device is named by its record's platform and the first
four characters of its install ID after the `in-` prefix. If that save fails,
nothing is written. Then it writes the text, reads it back and requires the
head's fingerprint, marks the bytes as imported so the legacy automatic send
does not echo them, and only then commits the adopted config data. The result
says whether the History copy can be restored and whether other offered
versions remain, so the box can offer Undo and say what the next check will
do when the commit fails. The file is never written with bytes that would not
survive the text conversion exactly. If the state commit fails or the process
dies after the file write, the next check finds the local file equal to that
head. When that head was the only offered content, the check reports Adopt and
records it without writing anything. When other contents remain, it offers the
same choice again with the written version as this device's side; keeping it
then publishes the same parents the original choice promised. Adopting without
a write commits the new config data directly. Finally the step plans once more
on the new state and reports the verdict a new check would show, whether that
plan proposes a publish, and with which parent keys. After a remote pick this
is exactly the publish the choice promised. After keeping this device's
version it can add the base as a parent, because the adoption of any
same-content heads has already moved the base. The publisher must find the
same parents on the click. The same file restores a History entry for Restore
and Undo: it saves the current file to History, labelled "Before restoring
the version from <time>" or "Before undo to the version from <time>", writes
the entry, reads it back and checks the fingerprint. It changes no sync
state, so the next check reports local changes. An entry recorded for a
missing file is refused, because restoring it would mean deleting
`settings.toml`. An entry whose bytes are not valid UTF-8 is refused before
anything is saved or written, because the settings writer takes text and
could not reproduce them exactly.

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
exposes these helpers through JNI; joining sync on Android calls the space ID
formatter, and nothing calls the comparison yet.
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
account share the pending attempt. Telegram's own Retry action now restores
that receipt only for a failed outgoing settings document staged under the
account's Purple staging directory and sent to its own Saved Messages. Since
Android `8d6bc3a8` that directory is `purple-sync/` in the app's external
files directory; copies an older build staged in Telegram's cache are still
accepted. A successful
retry records the server message ID and the sent fingerprint only if the
current settings still match the staged bytes; a failed
retry changes neither. Telegram's automatic resend at app start, after the
app was closed during a send, now gets the same receipt (Android `5c687693`);
a live emulator test on 2026-09-30 confirmed that the resent document recorded
the fingerprint and offer watermark and was not offered back for import. A
later emulator run verified a manual Retry of a message in Telegram's failed
state (forced by making the staged copy unreadable, then restoring it), a
restart resend from the new staging directory, and delivery of a copy staged
by the older build after an in-place upgrade. A lost server
response remains ambiguous. The new sync engine needs durable reconciliation
rather than relying on this legacy callback indefinitely.

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

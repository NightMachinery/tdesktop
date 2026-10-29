# Typed sync-chat I/O

The modern read adapters take `DesktopSyncChatIo`: one captured account,
environment and login, a canonical typed chat, its matching native InputPeer,
and fresh mapper facts. Read identity and posting eligibility are separate.
Missing access hashes, aliases of Self and mismatched input types or ids refuse
before a request. A known typed protected or otherwise ineligible O remains
readable for carry reconciliation; this never treats protection as ChatGone or
authorizes a write. Outward requests still require posting eligibility. A caller also
supplies an observer-free current-operation predicate. Polling that predicate
must not cancel or destroy the adapter. The adapter checks its captured session
and account-settings target before requests and callbacks, and again after a
download. Logout, Work Mode, source or run replacement makes the work incomplete.
No active-account fallback is used.

## Read route

The explicit typed constructors of `SyncHistoryScanner`, `SyncCandidateReader`
and `SyncAccountInventory` share one unchanged route. History uses that chat's
InputPeer. Exact channel reads use channels.getMessages with its matching
InputChannel; Self, User and basic Chat use messages.getMessages. Returned
message id and typed peer are checked independently of request construction.
Unknown constructors, inexact/flood or contradictory counts, wrong target/id,
transport failure and capped or unfinished scans cannot prove absence. Modern
full inventory permits fewer than 1000 pages and checks exact terminal evidence
before it can be Empty. Reaching page 1000 is Unknown, including a terminal reply
on that page. R1 global mark search retains its separate 20-page bound. Q remains a hint before complete inventory.

`DesktopSyncChatInventory` carries the route beside the unchanged core inventory.
The service checks the captured origin and exact typed chat before converting
it into resolver evidence. A naked core result or a matching numeric message
id from another chat is insufficient. Follow, Fill and winner-inventory work
still preserve the original cache and never commit a moved stream.

Fresh candidate metadata applies C0 again before download. The file-reference
refresh origin is FullMsgId(actual typed peer, message id), and the downloader
uses the captured account. The received body must have exactly the declared
bounded document size. Cancellation, loader error and incomplete bytes refuse.
The typed exact-read helper checks metadata, downloads the record and rereads
metadata in the same chat; document id/edit date changes keep the result unknown.

## Ledger and compatibility

A message id in the local ledger belongs to the enclosing store's chat. Version
1 implies Self; version 2 must match its explicit chat. Store readback confirmation
and absent-entry retirement require that exact chat before touching the ledger.
Their default argument is Self, so an old caller cannot consume a non-Self v2
id accidentally. The core's existing current-head deletion refusal is retained.
No schema migration or per-message chat field is added.

The original constructor overloads remain explicit Self paths for existing
legacy callers. Raw inventory-based review/publishing refuses a non-Self v2
store. The typed manual publishing seam is a separate route; its caller must
carry the inventory route through the existing binding, recovery, staging,
queue and fresh before-post gates. This does not execute an established-stream
move or supply setup UI.

## Qualification boundary

`purple/test_sync_chat_io.py` builds a small standalone program against the
existing generated TL schema. It compiles real route helpers, metadata, model
and store code, and ports the literal full scanner, candidate, download and
exact reread callbacks. Session, transport and downloader deliveries are
controlled unit endpoints. Real-file cases use retained synthetic directories.
Request wire, channel identity, complete-only Empty, declared sizes, retirement
and wrong-chat ledger controls do not establish server or UI acceptance.

Compile with compiler:local, release it, then run the output executable:

```sh
python3 purple/test_sync_chat_io.py --build-dir EXISTING_DEBUG_TREE --output-dir UNIT_DIRECTORY --build-only
UNIT_DIRECTORY/test_sync_chat_io
python3 purple/test_sync_chat_routes.py
```

Native server calls, posting, setup execution and full Desktop builds require a
later GO. Automatic sync remains Self-only in the core and service. Typed manual
I/O support cannot enable automatic non-Self posting.

## Upload and confirmation

`SyncConfigUpload` accepts an explicit typed route. The existing 32 KiB parts,
10 MiB upload bound, file id, checksum, random id and uncertain outcome handling
remain in use. After all parts finish, it runs CheckSyncChatBeforePost, then
captures a service lease for the same account, environment, login, typed chat,
eligibility kind and reader snapshot. A changed or unknown route refuses before
send/edit. A borrowed account-lock proof must be observer-free and remain valid
while the resolver runs. Its lifetime follows the owner and store, while effect
callbacks separately require their exact execution ticket.

New sends set the generated sendMedia silent flag. The current Telegram schema
has no silent flag on editMessage; edits use its ordinary media/message flags
with the same typed peer. No extra flag is invented. Missing send-as evidence,
lost rights, unresolved discovery and account retirement remain posting holds.
Prior consent applies to known reader changes in the same chat. The device
persists a warning when new members are known, while unknown readers and lost
rights hold. The opening and fresh typed routes still match origin, chat and kind;
a route-compatibility check does not grant reader consent by itself.

Send receipts match random id, server message id, typed target and C0 ownership.
Ordinary and channel new/edit Updates, updatesCombined and the direct non-channel
shortSent form are handled. A channel shortSent, contradictory mapping, foreign
target or unsupported form is unknown. Updates are applied through the captured
session even if local completion has lost authority. A receipt is only a hint.
Before confirmation, the typed exact reader downloads the staged version and
checks fresh metadata again; the result must be owned, Present, have the exact
id, and equal the staged bytes. Candidate inventory downloads also seal their
metadata after bytes, so reconciliation cannot confirm a changed document.
Unknown outcomes retain the pending record for later reconciliation.

Counters and loaded send-queue checks use the exact typed chat within the
captured session. Cancellation stops further parts/dispatch and releases its
in-flight counter once; a late callback cannot resume or confirm the operation.
Typed publishing carries its inventory route into the existing account binding,
recovery, staging, queue and ledger checks. Legacy constructors explicitly use
Self, and the automatic service explicitly rejects v2/non-Self before effects.
The normal settings screen provides explicitly confirmed manual publication to
the selected non-Self chat through its worker-owned publisher. See
[sync-chat setup](sync_chat_setup.md#manual-settings-publication). Setup completion
alone sends no settings. Automatic non-Self sync and stream moves remain disabled;
native qualification requires a separate user GO.

`purple/test_sync_chat_write.py` compiles the real generated TL requests,
receipt mapper, store and metadata helpers. It ports complete production upload
and post callback bodies into controlled session, API queue, policy and reader
endpoints, including the post-write callbacks. The test supplies deterministic
entropy and synthetic Updates, never sends a request, and retains its real-file
fixtures. It exercises part order, final policy changes, silent wire flags,
channel/random-id receipts, normal Updates processing, cancellation, exact
readback and failed reconciliation evidence. This is source/unit qualification,
not server, application or UI acceptance.

## Carry I/O admission and identities

T5b adds pure carry request builders and a worker-owned authorization adapter.
It adds no move executor, UI callsite, Telegram request dispatch or cleanup
activation. `DesktopSyncChatDeleteRequest` builds one exact id, using
channels.deleteMessages for Channel and messages.deleteMessages(revoke) otherwise.
It never builds deleteHistory or a whole-chat delete. `cleanupEnabled()` remains
false. T5g uses a separate worker cleanup adapter that applies the core journal
deletion policy and keeps production activation off.

A typed candidate now retains `DesktopSyncChatRecordReceipt`: the captured route,
the actual final exact messages response and the classified downloaded bytes.
The final reread is required for all downloaded bodies, including newer-major
and supported-header unsupported records, rather than only current settings.
Sealing validates typed peer/id, final server date, C0 ownership, document id,
edit date, byte length, candidate filename/caption and full response flags/counts.
The receipt owns its TL values. Carry inventory conversion matches every scanned
id to exactly one sealed receipt and the unchanged shared candidate result, then
uses MakeSyncCarryMessage and SelectSyncCarryHeads. Unknown, partial, duplicate,
corrupt or substituted evidence holds; it never becomes Empty. Unsupported records
that the core allows to carry retain all raw bytes. The shared core structs and
state/journal hash meanings are unchanged.

`DesktopSyncChatCarryIo` uses the carry store's account lock, checked worker
thread, expected file bytes/identity and durable replacement barriers. `Reserve`
persists the accepted destination reader baseline and one identity per effect
before the executor may persist DeliveryPrepared or MarkPostPrepared. Each
forward slot binds its source message id and whole-record hash; its random id
is persisted once. Own and mark random ids must be canonical nonzero unsigned
64-bit decimal strings from the core journal's ownRequestId/markRequestId, and
must differ when a new mark is required. Upload file id is persisted alongside the own random id. IDs cannot
collide across slots or rotate on retry. Forward admission also requires a new
exact source receipt matching the plan's id, date, ownership and bytes; protected
O can only supply the permitted retained own version, never forwarding.

Only after the core send intent is durable can `Authorize` persist the slot as
issued and return an opaque `DesktopSyncCarryPermit`. Its private constructor
keeps ordinary publishers from manufacturing this bypass. It binds the operation,
revision, account, O/N, payload hash and IDs to fresh mapped peer, resolution and
reader evidence. A known initial reader disclosure must match the accepted peer
snapshot before the first write. Subsequent known additions keep the original
baseline and set a sticky warning; unknown readers or lost rights hold. A changed
winner, Unknown/Migrated R3, changed source record, login, account or typed peer
refuses new effects. The normal resolver input is retained and recomputed rather
than trusting a caller's success enum.

There are two login identities: the service route's login is a RAM UUID, while
the carry store's loginAuthority is persistent and derived for the account.
Fresh input supplies both separately. Persistent authority permits recovery in
the same login; the current route and callback fence bind each live execution.
Neither one is substituted for the other or logged. Store close/failure or a
core journal transition revokes published permits through a RAM epoch. Current
predicates inspect RAM only. The worker seals the own-record body into the
permit; upload parts and renewed GUI callbacks do not hash or read its source.

The carry overload of SyncConfigUpload accepts only an own-record permit and
its persisted file/random identities. Its existing shared request controller
still handles floods. Every definite deferred send renews R3/full-info and
source authorization through the caller's worker queue, producing a strictly
newer permit renewal number with the same effect identity. Returning the opening
permit refuses. Each completion also has a gate ticket; a duplicate or older
callback cannot finish or dispatch newer work. The GUI never reads the carry
journal, waits for disk or allocates a new ID on this path. Ordinary publishers
continue through their existing Use/service-lease gates. The later carry executor
must provide the asynchronous worker renewal callback; there is no production
caller in T5b.

A send acknowledgement is a hint. Updates apply to the captured session even
when UI/permit authority has gone; a stale completion cannot record success.
RecordReceipt may retain an exact request-bound acknowledgement id on the worker.
Issued or unknown slots found after restart cannot be authorized again until a
complete scoped destination inventory reconciles the exact record hash, with
the same random/file IDs retained. A valid mark read also needs that saved
acknowledgement id and exact owned text/date/peer/no-media evidence. Empty mark
searches or reads never prove the request absent; without retained request proof
an uncertain mark holds. T5b does not implement post-effect retargeting.

The private carry envelope has an additive `transport` object, version1, with
`entries` keyed by SHA256(kind + ':' + source_id + ':' + record_hash). Every entry
requires `kind` (forward/own/mark), `hash`, integer `source_id` (positive only for
forward), decimal-string `random_id`, decimal-string `file_id` (zero except own),
and `state` (prepared/issued/unknown/verified). Optional positive integer `ack_id`
is a request-bound response receipt. `readers` holds the original members,
other_members/other_admins count strings and warning boolean; `reader_warning`
is sticky. Unknown members are preserved; missing required fields, corruption, future versions,
noncanonical IDs or duplicate random IDs refuse without deleting the journal.
The root `transport_sha256` is SHA256 of canonical JSON containing
`domain` = `purple-sync-carry-transport-v1`, `operation`, integer core `mode`,
canonical `source` and `target` tokens, `account`, `binding_sha256`,
`login_authority` and the whole `transport` object. It checks accidental
corruption and cross-operation substitution; it is not a secret-key MAC.
An existing transport requires its matching checksum. An envelope with neither
field is the pre-reservation state. Neither core journal hashes nor shared state
versions change.

The existing 4 MiB journal and 1024-entry bounds remain. Switch reserves its
mark identity with the first delivery identity. Before issuing any permit, the
worker checks room for all required identities and their largest possible
positive acknowledgement ids. A near-limit unknown extension is retained, but
no effect is admitted if later receipt metadata would exceed the bound.

The registered carry-I/O host gate uses real generated TL wire constructors,
literal reader/uploader callback bodies, worker-owned real private stores and
controlled main-thread transport endpoints. It covers protected reads/no writes,
final metadata, equal-number typed peers, ID persistence, issued restart holds,
uncertain reconciliation, mark receipts, reader changes, durability faults,
multiple upload parts, cancellation, duplicate/stale renewal callbacks and late
Updates. Full shared host gates qualify the delivered tree. This does not establish
server forwarding semantics, native UI behavior or physical power-loss durability.

The carry uploader obtains its immutable payload from the worker-issued permit,
rather than accepting replacement bytes from an ordinary publisher. The permit
retains the core plan's validated own record. Fresh renewal compares the sealed
operation and hash identity; it performs no disk read or body hash in the GUI
callback. The canonical mark text is the existing setup mark text, shared by
the builder and exact readback. Follow and Fill retain the existing mark and
do not require or manufacture a new mark-send identity.

Before any delivery permit is issued, every delivery identity required by the
core journal has to be reserved. A journal or transport size refusal therefore
happens before the first outward effect, rather than midway through allocating
the remaining forward identities. The pure cleanup builder still grants no
cleanup capability.

Live slot ownership is shared by wrappers of the same worker store. Another
wrapper cannot reconcile an active issued slot. Destruction revokes only that
wrapper's permits, so an old wrapper cannot cancel a newer owner's permit after
an explicitly reconciled retry. Store close still invalidates every generation.

The automatic host driver compiles `purple_sync_config_text.cpp` and
`purple_sync_config_review.cpp` into its own output using each unit's exact
Debug recipe, including its flags, include roots and PCH handling. These units
depend on the typed reader headers; reusing unrelated Debug versions would make
the gate stale. The shared runner's compile wrapper and depfile checks apply.
Other unchanged native support objects remain reusable only while the runner
verifies their inputs and Ninja reports no pending work.


T5g adds an injected cleanup adapter in purple_sync_chat_cleanup.*; production
activation stays OFF. It uses the core's frozen cleanup candidates and exact
record/old-mark authorization, never the standalone stale-mark exception.
Complete N coverage and a fresh positive owned O read precede each single-id
request. The durable scoped ledger records issued intent and explicit same-id,
matching-peer absence. Peerless/zero-row/inexact responses remain Unknown.
Acknowledgement counters are applied before exact reconciliation, with a worker
renewal before every deferred replay. Superseding an item permit does not end
the operation: a bounded renewal timeout finishes into a recoverable hold once,
and late grants cannot send. See sync_chat_move.md for progress, partial results,
Retry/Skip and the ledger shape. No native or server execution is qualified.

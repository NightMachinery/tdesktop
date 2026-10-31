# Moving a settings sync chat: local storage
A Plan reply classifying O as ChatGone must also carry its affirmative scoped
old-chat direct-read result. A readable, unknown, migrated or merely missing-mark
read cannot authorize planning without O's records. The executor holds before
any reservation or outward effect when these two pieces of evidence disagree.


Desktop has bounded storage, typed effect capabilities and a serialized Carry
executor. The shared `NextSyncCarryJournalAction` selects every move phase;
Desktop supplies only durable file receipts and injected typed I/O. There is no
production network backend, Carry screen or automatic non-Self posting in this
slice. The foreground UI belongs to T5d and cleanup policy activation to T5g.

## Retained base format, shared by clients

The shared local state remains version 1 (implicit Self) or version 2 (typed
chat). There is no Desktop version 3 and no core codec change. The core preserves
an optional top-level `retained_config_bases` object alongside its known fields:

```json
{
  "retained_config_bases": {
    "version": 1,
    "records": [
      { "sha256": "<64 lowercase hex digits>", "record": "<canonical record JSON>" }
    ]
  }
}
```

`record` is a JSON string containing the complete canonical UTF-8 settings
record, including its envelope, exact settings text, causal version and lineage.
`sha256` hashes those UTF-8 bytes, before JSON-string escaping, using SHA-256.
The record must parse as a supported settings record and its space must match
this state. A base lookup requires both the exact version key and exact lineage.
A matching text fingerprint alone is insufficient. An adopted remote version
can be a base; its writer does not become this install's writer.

Each record is at most 262144 bytes (256 KiB), measured before string escaping.
There are at most 16 distinct hashes and at most 2097152 total record bytes.
The complete serialized state must also fit the existing 4194304-byte core
limit. These are independent bounds: JSON escaping can exhaust the state limit
before the raw-record total. Oversize records return `BaseRecordTooLarge`;
count, total or encoded-state exhaustion returns `RetentionFull`. Neither path
truncates records, drops unknown fields or partially updates the state.

Version must be a JSON number exactly 1. Numeric future versions, including
2147483648, return `NewerRetention`; missing/invalid required members, duplicate
hashes, bad hashes or invalid records return `InvalidRetention`. Unknown container
and record-entry members are preserved. A missing subobject is a legacy state,
not an empty retained base. A write that needs an unavailable base returns
`MissingLocalVersion` before stage creation or settings replacement.

Confirmation retains the exact staged/read-back record in the existing atomic
state write before removing the stage. Adoption supplies a verified raw record
from the selected inventory. Bootstrap accepts only a complete matching record
with the old key, lineage and space; it does not rename current or dirty settings
as the old version. The bounded History list is not a retention guarantee.

The ordinary publishing worker materializes retained entries as private immutable
`retained-bases/<sha256>.json` files. Only after checked file and directory
barriers does it compact ordinary embedded entries to the current base. Entries
with unknown members are kept. Automatic checkpointing is allowed only on a worker while idle, with no
pending state image. Production automatic transitions currently run on the GUI
thread and defer this checkpoint. Their bounded embedded entries remain intact
until a serialized worker executor can materialize and compact them. Its next exact expected state is decorated before the auto
record is saved; execution still compares the complete canonical bytes. No field
is excluded from a journal hash. There is no object garbage collection in T5a.

## Carry journal and immutable images

One worker owns the ordinary account `QLockFile` and the cached local state.
Ordinary open refuses `CarryPending` whenever `carry.json` exists or cannot be
safely identified, before it can retire pending files. Carry open uses that same
lock and reads state without ordinary stage cleanup. There is no public
skip-validation flag. Recovery journals continue to hold admission.

The private `carry.json` wrapper is bounded to 4194304 bytes and has required
fields `version` (1), `account` (canonical environment/account key),
`binding_sha256` (SHA-256 of the exact account binding token bytes),
`login_authority` (the persistent 64-hex login binding also used by the account's
sync-chat cache), `operation`, `journal_sha256`, `journal` and `objects`.
`journal` is the shared core journal object; its canonical bytes determine
`journal_sha256`. `operation` must equal the core operation id. `objects` maps
64-hex byte hashes to one of `state`, `record` or `text`. Unknown wrapper and core
members survive rewrites. Future wrapper/core versions refuse, without fallback,
deletion or an attempt to resume under a new login.

Images live under `carry-objects/<kind>-<sha256>.json`. State images are bounded
by 4 MiB; record and retained-text images by 256 KiB each. At most 1024 images and
64 MiB aggregate bytes are admitted across the account, including retained operations. A hash collision with different
bytes/kind refuses. Files are private regular files opened without following
links, checked for ownership, permissions, link count, expected bytes and parent
identity. Only fixed kinds and lowercase hashes form paths.

Old, reserved and next state images are stored before the journal can refer to
them. Exact base records remain in embedded retention or immutable carry objects;
preparation refuses if the new state's base cannot be recovered from either.
The adapter does not decorate a core-prepared carry state after its hash has been
chosen. Stage reservation, confirmation and move use the exact core image.

Only the adapter can supply successful `objectsDurable`, `StateObserved`,
`OwnStagePersisted` and `StageRetired` facts. Plan/pending preparation stores the
required images first. Destination verification uses `PrepareSyncCarryJournalCommit`;
cache receipts require the matching account, persistent login, operation and
mark, then actual barriers on the private prepared or admitted cache file.
Cache receipt verification is read-only: the later cache writer must merge
consent and sticky warnings, never replace discovery's accepted baseline.

A replacement checks captured identity, parent identity and exact old bytes,
writes a private temporary, flushes it, rechecks immediately before atomic rename,
and checks file and directory barriers before reporting success. This is a
cooperative account-lock compare-and-swap, not a claim of atomic protection from
an unrelated malicious process. Matching bytes after a failed barrier are not a
durability receipt. Restart re-establishes barriers before asking the core to
resume. Rename before journal acknowledgement therefore preserves the saved
sequence and the core's read-before-resend action. Stage retirement verifies the
matching committed state image, removes only the exact authorized stage, and
checks its directory barrier. Other stages hold recovery.

On POSIX the worker checks `fsync`, and macOS also checks `F_FULLFSYNC` on writable
file handles. Windows checks `FlushFileBuffers` on handles opened with write
access and never flushes a read-only directory handle. The conservative Windows
directory barrier needs a writable volume handle; insufficient rights refuse
`DurabilityFailed`. A supported non-admin directory-durability protocol remains
a platform follow-up. T5a does not claim Windows or physical power-loss
qualification from macOS host tests.

Logout revokes the executor's RAM authority immediately and queues worker
retirement. It never waits for disk work. The journal is outside the admitted
cache. A core version-2 journal binds a 32-hex login authority derived as the
first 32 hex digits of SHA256 over `purple-sync-carry-login-v2:` followed by the
64-hex cache login binding. The wrapper retains that cache binding. Missing or
inconsistent login evidence refuses; an old version-1 journal cannot authorize
effects. A changed login can inspect an exact same-account journal but cannot
resume or send. Explicit `AbandonInterruptedOperation` preserves current state,
pending bytes and unresolved effect identities. If upstream logout erased the
binding preference, the worker returns the retained state for the separate
explicit binding-recovery flow; it never silently writes a preference.
Upstream `main/main_account.cpp` calls `local().reset()` in logout and
`storage/storage_account.cpp` clears account preferences in `reset()`.

A terminal journal is first persisted in `carry.json`, then copied with checked
barriers to `carry-history/<sha256-of-wrapper>.json`. Only after that receipt does
the worker remove the active marker and check its directory barrier. A crash
before removal keeps ordinary writers held. Restart re-establishes the barriers
and finishes archival idempotently. Archives retain unresolved ids and are never
automatically pruned. There are at most 64 archives and 64 MiB total archive
bytes. Discovery validates account, binding, login evidence and content-addressed
names, deduplicates byte-identical operations, and refuses conflicting duplicates.
The snapshot exposes both parsed core journals and their immutable wrapper
images, including every saved transport slot, random id, file id, acknowledgement
and unresolved outcome. These images are recovery evidence, never a posting
capability.
`RemoveAccount(true)` is a worker-only account-removal seam: under the account
lock it validates all owned carry files before removing any. It leaves ordinary
state/stages to the encompassing account-removal operation.

## Threads and inherited writes

New carry and durability operations refuse on the application main thread.
Publisher preparation, stage/confirmation and base checkpointing run on the
landed publish worker. Automatic `AutoSession::Event` calls `Transition` directly
on the GUI thread (`purple_sync_auto_service.cpp:257`, `:673-675`); session-ready
work also uses `crl::on_main` (`:638`). This inherited path retains the bounded
embedded records but skips the new worker-only checkpoint. The checkpoint API
still returns `WrongThread` on the application thread. A serialized automatic
worker executor is deferred to its executor slice.
Manual apply still inherits synchronous GUI calls from
`purple_sync_setup_box.cpp:339`, `:553` and `:759` into `ApplySyncConfigChoice`.
The existing state writes in `purple_sync_local_store.cpp:998` (stage),
`:1091` (confirmation) and `:1138` (adoption) now include
the bounded retained records. These inherited calls can carry a larger payload;
T5a adds no separate synchronous pin barrier or main-thread fsync. The exact
post-change write-site lines and validation receipts are recorded in result.md.
Moving manual apply fully to a worker is a separate follow-up.

## Qualification boundary

`purple/test_sync_chat_carry_store.py` compiles the production storage adapters
against the existing Debug objects, without an app build or relink. Its private
synthetic file cases cover retention formats and bounds, unknown preservation,
legacy bootstrap, dirty settings, lock/admission fences, checked barriers,
replacement races, carry commit/stage retirement and restart at rename boundaries.
The existing local-store, automatic, publish-flow and full Purple host gates
qualify ordinary sync behavior. No test makes Telegram calls, changes a native
profile or demonstrates a physical power-loss guarantee.

Automatic regression cases run on the `QCoreApplication` main thread used by the
production service and again with `--worker`. Main-thread checkpoint refusal
must leave state bytes unchanged and create no immutable-base directory, while
Enable and the other ordinary transitions continue normally. The worker variant
also verifies compaction before retirement.

## Executor and recovery

`DesktopSyncChatCarry` runs only on a serialized worker. Its public service
captures the opening account, login, binding and source settings. Each command
carries a random run, ticket, operation id, journal revision and typed O/N routes.
Late replies cannot finish or cancel another run. GUI callbacks check only RAM
admission. Raw Updates are applied before the reply's local-owner check.
Cancellation revokes admission first, queues worker retirement, cancels transport
and releases the account lease without a GUI wait. An outstanding request's
uncertain journal intent survives.

The account RAM gate rejects new publishers and revokes existing writer leases
before Begin. Automatic, setup, legacy and selected-chat publishers receive the
quiescence event. Begin waits for existing worker leases to drain and still takes
the ordinary account file lock. The registry mutex protects lookup only; no
callback, file lock or worker operation runs under it. Writer counts and the
carry epoch are atomic. A late lease cannot reopen a successor epoch.

The action order and restart behavior are:

- ResolvePending retains exact stage bytes. Readable O needs complete owned
  readback through the ordinary pending protocol. Protected O uses the shared
  NotIssued/Issued rules; incomplete evidence holds. ChatGone is affirmative.
- WritePendingState, RetirePendingStage and ResolveAgain use exact core images
  and fresh resolution. Restart never infers absence from an error.
- Plan uses exact inventories and the retained base, never dirty settings text.
  StageOwn persists the core's reserved sequence and immutable record.
- Deliver persists stable effect identities and DeliveryPrepared before exposing
  any permit. Definite retries renew fresh effect-specific gates and retain ids;
  ambiguous outcomes go to complete typed inventory reconciliation.
- VerifyDestination uses authenticated record receipts and the shared commit
  preparation. Playlist status defaults to NotInUse; a required playlist without
  the future receipt remains held.
- PostMark persists MarkPostPrepared before dispatch. ReadMark requires exact own
  canonical text and the matching saved request receipt. Ambiguity cannot invent
  a mark id or cause a blind repost.
- PrepareCache writes a private receipt. CommitState writes exactly the core's
  next image. RetireOwnStage installs any deferred target pending bytes only
  after that state is committed.
- Cleanup accepts only core-verified empty work in this slice. Deletion policy
  dispatch remains held; explicit SkipCleanup is a core transition.
- CompleteCache merges preserved fields, accepted baseline, consent and sticky
  warnings. Done is archived before ordinary admission resumes. RepairMark
  repairs only the mark/cache and leaves the established settings state intact.

The host executor fixture uses real private files and injected raw TL replies.
It covers action-boundary restarts, all six durable fault points at every write,
unknown effect outcomes, protected pending cases, fresh login discard, stale
callbacks, cache merge, account removal, main-thread refusal and two-thread RAM
gate contention. These are source/host checks, not native Telegram qualification.

### Finishing a readable O pending record

The wrapper records `opening_pending_sha256` and an immutable copy of the exact
opening pending record before Begin is durable. This client field does not alter
core state versions or journal hashes. It is mandatory during ResolvePending; a
missing, changed or unreadable stage refuses. Core `pendingStageHash` is populated
only after confirmation, so it is not used to authorize the earlier upload.

A complete authenticated inventory of O must first prove the exact record absent.
Only the owned Carry worker can issue the private pending permit for the journal's
current operation and revision. It binds the account, login, exact typed O, exact
bytes, current source eligibility and admitted mark/reader consent. Stable random
and file ids are durably saved before dispatch. Copies share one upload claim;
ordinary SyncConfigPost and other publishers cannot obtain or reuse it, and the
Carry RAM gate continues to refuse their normal paths. An acknowledged record
that is still absent holds. An ambiguous effect rereads O before any continuation.

The permit does not finish the core action. Only the normal owned exact readback
can prepare the pending state, after which the executor follows the shared core's
barrier and retirement actions. Protected sources never obtain this permit.

Bounded journal validation uses compact JSON as its intermediate representation.
A wrapper already just below the storage limit must remain readable; indentation
must not turn it into an oversized input. Any later receipt that would exceed
the bound is refused before dispatch and leaves the existing bytes intact.


### T5c recovery evidence boundaries

An issued pending O send is a durable lower bound on the evidence. The client
transport wrapper's issued, unknown or verified pending entry cannot later be
reported as NotIssued. An issued-intent observation imported from an ordinary
publisher is persisted from an accepted ResolvePending reply before its overall
outcome is tested. The run, ticket, operation, revision, account/login and frozen
O/N must match the current command. An Unknown or Failed reconciliation with
an affirmative Issued observation retains that lower bound, even when no O
inventory is available. A stale command cannot install or clear this evidence.
A failed persistence reports a held operation rather than advancing it. On a protected O, an issued intent requires a complete,
account/login/chat-scoped O read before the core can defer or finish its stage.
No read, partial reads and uncertain absence hold with the original state and
stage. A definite ChatGone still uses the separate shared abandonment rule.
The transport metadata codec and digest are shared by the store and I/O layer.

On restart, the resolver must see the accepted mark, rather than replaying the
opening winner. A changed winner can permit reconciliation only for frozen N
and the exact retained operation. A scoped read-only lookup may obtain its
retained acknowledgement; advancement requires that acknowledgement and exact
owned canonical mark id/date/text readback. A different winner, mismatched
receipt or unavailable proof holds. This recovery route cannot reserve or send
another delivery, pending post or mark. After a verified mark it may finish the
local state/cache steps against the current N resolution; it never retargets.
Follow retains O as its read pointer until commitment, which is distinct from
its current winner N.

New-login Discard is explicit and does not resume network effects. When the
already prepared N image has reserved a deferred pending sequence but its
ordinary file is missing, the worker first validates the shared abandonment
transition with the exact retained target bytes. It durably installs those same
reserved bytes, confirms the unchanged N state, and retires only a byte-exact,
immutable-object-backed obsolete Carry stage authorized by this operation.
Both old-stage-present and already-retired windows are supported. A retry after
a failed write, rename or barrier confirms the same file, never allocates a new
sequence. A conflicting target, changed obsolete stage, unknown object or orphan
file refuses without terminal abandonment. Other unpaired local-stage shapes
remain held for review; Discard is not a general orphan-file deletion tool.
Only after the ordinary pending file is durable and the directory is consistent
does core abandonment become terminal. State and all external-effect identities
remain in the retained journal wrapper. This establishes process-crash recovery,
not a physical power-loss qualification claim.

During committed Cleanup, affirmative scoped O ChatGone reaches the shared
CleanupChatGone transition. MarkGone, protection, unknown reads, wrong account
or chat and uncertain absence do not. This sends no deletion request and does
not enable the later cleanup policy. OwnRecord uploads use the same one-use
shared claim as pending uploads: copies and Renew share it. A second uploader
cannot consume the old authorization. Complete reconciliation may authorize a
fresh retry with the same durable file and random identities; the old claim
and capability remain unusable.


### Fresh reconciliation evidence

Each client transport slot keeps a canonical unsigned decimal `generation` in
its hashed transport entry. It advances durably on issuance, uncertain outcome
and accepted reconciliation; Renew preserves the attempt and stable request/file
identity. A missing legacy generation is zero, malformed values refuse, and
uint64 exhaustion holds without wraparound. Receipt-capacity admission budgets
the largest encoded generation as well as the acknowledgement.

A typed inventory records an immutable, process-scoped monotonic stamp before
its history scan starts, retained through downloads and completion. The owned
store shares each slot's current generation and outcome fence across wrappers.
Reconciliation accepts only a scan begun after that fence, then consumes it by
advancing the slot generation/fence. An empty inventory from before retry B's
uncertain outcome cannot clear B, even if it already cleared A. The same proof
cannot be consumed twice. Pending O reconciliation uses the same chronology.

Inventory stamps have no persistence/deserialization API. Opening the store
creates a new read fence, so an old inventory is refused after same-process
reopen and evidence cannot cross process restart. The durable slot generation
survives both. A genuinely new complete scan restores reconciliation; no core
state/journal format or hash semantics change. This is the same ordering rule
as Android's generation-bound read tickets, expressed as a scan-start stamp and
per-slot store fence rather than a transport-specific ticket.


## Explicit Change sync chat UI

T5d adds a bound-move chooser alongside T3 fresh setup, with activation off.
The action captures the opening account/login/window, exact worker-published
state bytes and admitted cache, the old chat and resolution winner. Both target
preview and fresh acceptance use the same core eligibility predicate. The
settings/reader/cloud disclosure is explicit even for Saved Messages. The old
chat remains, and only the optional own-settings/mark cleanup is offered. Its
default is off for an old Saved Messages source. In both Create and Existing
confirmations, an adjacent warning explains that older Purple builds still read
settings from Saved Messages and may pause or offer to start over if those
messages are removed. This warning depends on the old source, never the target.
No whole-chat deletion is implied. State/cache/winner and reader changes invalidate the confirmation.
An inactive or closed owning window and passcode lock revoke it immediately.
All comparisons are in RAM on the application thread.

The production service stops after qualification review. It does not construct
T5c's worker or acquire its Carry lease, and does not start a network transport.
A held confirmation is not a durable operation, a successful move, or authority
for background work. Source-only tests establish the held UI/model and durable
provision adapter, not native Telegram behavior. New native activation needs
an explicit owner GO and a reviewed transport hookup.

For a new destination, the purpose-tagged T3 provision sequence retains the
create operation and exact matched-request proof, but produces no mark or
cache. It verifies fresh full info around a complete Empty target inventory.
Uncertain creation after restart reconciles or holds without duplicate create
or orphan deletion. Carry must complete verification, mark readback, local
state and cache commitment before T3's created-channel-only silent pin, mute
and archive policy can be used. Provisioning alone is never shown as committed;
existing/Self targets cannot take that decoration route. Postcommit cleanup
failures retain T5c's checkpoint and continue to require explicit Retry or
Skip cleanup, not another fresh choice.


The dormant `DesktopSyncChatMoveDecorationRequest` builder accepts only a
completed provision receipt and a valid committed Carry journal for its exact
opening state, account, domain-separated Carry login authority, source and created
channel. It reuses T3's silent
pin, mute and archive request shapes with the Carry mark id. Precommit,
provision-only, existing-chat and Saved Messages receipts refuse. There is no
production dispatch callsite while move activation is off. Activation must wire
this builder after durable commit and renew its foreground and scope authority
before each effect; provisioning alone grants no decoration permission.


The dormant decoration action distinguishes NotStarted, Ready, Waiting, Held,
Complete and Cancelled. A failed pin, mute or archive callback keeps Held and
its exact step across unrelated Carry Cleanup/Done publications. A waiting
callback similarly admits no second command. Only explicit Retry renews
admission for the failed step; earlier successful decorations are not replayed.
Run, operation, ticket and step fence stale callbacks. The same committed
receipt stays bound throughout. No production UI or service constructs this
action while move activation is off; native activation must retain one action
per operation and renew the existing foreground and request gates.

## Follow/Fill consent and the first effect

Desktop T5e reuses this Carry executor for resolver Follow and Fill. The
production activation constant remains off, and the real setup screen exposes
no actionable Follow/Fill or Pause/Resume control. The injected host flow
resolves and checks fresh full peer information at acceptance and again at
final admission, bound to the opening window, account/login, exact O/N, winning
mark, stream and state/cache bytes.

Follow reconciles its own late O posts before changing the local binding,
including an implicit-v1 Saved Messages source. Fill reads and verifies the
recovery source before copying into N. A complete readable Empty source is
different from an unreadable or incomplete source, which holds. Fill with no
local state leaves that state absent. These are existing core transitions,
not a second client move state machine. Settings Carry retains the
playlist-required refusal where playlists are in use; this slice does not
implement a playlist transport.

D-M2B5-9 fixes the accepted N readers and disclosed stream before the first
outward record delivery or mark intent, whichever is first. The client writes
its original hashed roster and known-roster flag into the authenticated
transport envelope before any reservation. Journal creation also records that
the operation requires this snapshot, so a restart in the intervening window
holds instead of manufacturing consent. A retry/reopen preserves that exact
baseline. Counts, eligibility and the shared core journal version/hash
semantics stay unchanged.

Before intent, a newly added reader or changed initial disclosure holds for
another foreground choice. After intent, known additions warn and continue
under C6, with a durable sticky warning; unknown readers and lost rights still
hold. Compare member identities only when the original roster was known.
Count-only originals retain their count-based detection and same-count
replacement blind spot when a roster later becomes available. Departures alone
do not warn. Completing a same-chat cache merges disclosure streams and
warnings without overwriting that original baseline. An old different chat's
disclosure does not transfer to N.

Pause is local to this account/device and covers every stream. Logout keeps
the negative choice in an account-only envelope, never the old login's reader
or move authority. Explicit foreground Resume alone clears it. The unchanged
D-M2B5-11 interrupted-journal rule still refuses a different login even after
Resume: a retained operation is Discard-only, with its unresolved effects
preserved. The worker performs exact-snapshot cache changes; no disk wait is
added to the GUI. Production activation and server behavior are not claimed by
the host fixtures.

A deferred Follow/Fill opening retains the complete accepted admission: start,
original peer/roster, disclosed stream and exact state/cache byte witnesses.
Retry uses that same admission; it never substitutes retry-time consent. A
publisher changing either local image or the original readers therefore holds
before the first outward intent. Successful replies must pass the persisted
reader-authority guard before mark/cache/commit processing. Cache writes also
check original-known roster and creator/admin authority directly. An issued
receipt remains retained when later reader authority is lost, and reopening
cannot replace that baseline to make the operation proceed.

## Explicit old-settings cleanup (T5g, qualification only)

Cleanup remains OFF in production. The injected Carry executor may clean only
its frozen O after verified delivery to N, durable state/cache commitment and
recorded authorization. `SyncCarryCleanupCandidates`, `CheckSyncCarryDeletion`
and `CheckSyncCarryOldMarkDeletion` are the only deletion authorities. There is
no whole-channel or history deletion builder.

AllSettings needs the accepted cleanup checkbox; Saved Messages starts unticked
and retains the older-build warning. The old mark has a separate unchecked
choice, stored in the core's `removeOldMarkAuthorized`. Follow selects only the
core's OwnLatePosts outside Saved Messages. Fill grants no cleanup. Ordinary
messages, legacy settings.toml documents, foreign records, changed bytes/dates,
other installs' late posts and records without N coverage are not broadened into
cleanup candidates.

Each single-id request has a new worker-issued permit after an exact owned
readback with the original hash, date and classification, plus a complete typed
N inventory. Issued intent is durable before a request exists. Acknowledgement
counters must be applied before reconciliation; acknowledgement alone never
marks an item absent. Deletion uncertainty holds until a fresh scoped exact
absence is proved. Empty result vectors, search omissions, incomplete reads,
peerless messageEmpty and contradictory peers remain Unknown. The standalone
selected stale-mark exception does not apply to cleanup or the Switch old mark.
A definite scoped ChatGone uses core CleanupChatGone; protection or lost rights
never becomes ChatGone.

The cleanup ledger is version 1 under the Carry transport's `cleanup` object,
with `entries` keyed by a domain-specific hash of item kind, id and record hash
(or old-mark date). Each entry stores kind, id, decimal date, hash, state
(issued/absent), and the body_sha256 of an owned retained text/record object when
positive evidence exists. The existing transport hash binds it to account,
login, operation, O and N. Unknown future ledger versions, substituted bodies,
foreign references and size overflow refuse. Unknown fields remain preserved.
The existing four-MiB journal and 64-MiB object limits apply, never truncation.
Confirmed results survive restart and partial completion; Skip passes them to
core SkipCleanup so only remaining references are abandoned.

Progress says “Moved; old settings cleanup is incomplete” while a durable hold
remains. Retry and confirmed Skip stay available after I/O failure, uncertain
responses and slow renewal. Remembered phase/counts are presentation only;
recovery reopens and validates the actual journal and committed state/cache.
Only a first start compares the original chooser's exact file images. A failed
or waiting Pin/Mute/Archive keeps its step and reason while cleanup receives its
latest controls; status updates never implicitly retry that decoration.

Operation liveness is separate from an item permit's superseded generation.
Every deferred deletion renews full reader/N and exact O evidence through the
shared request controller. A bounded renewal timeout completes once into a held
state, with late grants fenced. Copied permits share one request claim. Close,
logout, wrong account, stale operation/revision and new runs cannot use them.
The GUI reads RAM capabilities only; all journal, cache and record work stays on
the existing serialized worker.

The install-moved note is based on a genuine own config record in N with this
install/device, excluding required carried hashes and the operation's own carried
record. Copying historical records is not evidence that this install has begun
using N. The OFF-gated progress box has Retry and a separately confirmed Skip;
a stale box cannot act on a successor operation. Host tests do not qualify native
UI, Telegram server behavior or physical power-loss durability.

## Lost recovery and same-chat mark repair

Recovery is a separate bound-account flow. Its Create, Choose, Mark this chat
again and Retry recovery controls, public selection, click handlers and nested
confirmation are all behind a constant OFF gate. The status remains a
non-actionable qualification hold. Host injection qualifies the orchestration;
there is no production recovery transport callsite or activation in this slice.

A missing mark is not a lost chat. Searching keeps the existing 30, 120 and
600 second retries. Reconnect and explicit retry resolve again. A failed request,
timeout, incomplete search or unknown direct read never establishes ChatGone.
The recovery opening captures the account, login, settings binding, original
cache, exact state and cache images, source, resolution and window. Every fresh
resolve must match before provisioning and before Carry. Revocation, another
winner or changed opening holds without retargeting.

Create provisions only through the existing journaled setup path. Its result
must be a completed, matched-request-created channel for this exact source
fingerprint. It cannot post a mark, decorate or create an admitted cache itself.
One controller starts provisioning once; an uncertain result stays in that
journal for explicit reconciliation, never a second blind create. Choose uses
the same eligibility and disclosed target snapshot as Change. Both enter core
Switch through the existing serialized Carry worker, never fresh setup for a
bound account.

When O is protected or definitely gone, Carry uses only this install's verified
retained own base. Missing or mismatched bytes yield MissingLocalVersion. Unsynced
editor text is neither a substitute base nor a new identity for an old version.
Protected pending sends still use ReconcileProtectedStageForCarry: retained
Issued intent cannot be downgraded, and a complete inventory must begin after
the latest uncertain outcome. ChatGone uses AbandonStageForLostChat. Unknown
reads hold. Fill still requires a complete readable source.

Mark this chat again uses core RepairMark with O=N. It requires the positive
captured mark's fresh R3 MarkGone, no winner, a readable eligible chat, the
original settings reader consent and known current readers. The original reader
snapshot remains authoritative: known additions warn and continue, unknown
population, loss of a previously known roster, or lost creator/admin rights hold.
Desktop caches now retain the kind and admin facts when they are known. A legacy
cache missing those applicable facts holds mark repair instead of inventing
original consent; another destination can still be reviewed. Self and private chats do not have
creator/admin roles. The cache fields are `reader_kind` (the shared kind enum
integer or null) and `reader_admin` (boolean or null); malformed or unsupported
values refuse cache admission. Existing same-chat baselines are not backfilled
from a later reader snapshot. Existing baselines and sticky warnings are never overwritten.

The repair store refuses pending or orphan stages. It retains the exact existing
state image, and its commit performs a durability barrier on that identical image
instead of rewriting state. Binding and the settings/version ledger therefore
remain byte-identical. No settings plan, own record, cleanup, deletion or fake
O-to-O Switch is created. Only a replacement mark and its admitted mark cache
change. A lost base alone does not require inventing one merely to repair a mark;
invalid retained-state formats still refuse. Late direct-read, winner or reader
changes must pass the real per-effect Carry gate before a mark can be issued.

New recovery disk and journal work runs on the serialized worker. The existing
setup screen still validates its opening target and snapshot synchronously:
`purple_sync_chat_setup_box.cpp:660-661`, `MoveContextCurrent` at line 206 and
`Current` at line 216 reach `ValidateAccountSettingsOpenTarget` and
`SyncChatSession::Setup` (`purple_sync_chat_service.cpp:1095`). Their existing
filesystem validation is in `purple_account_config.cpp:985` (`validateOpen`)
and line 1977 (`operationTarget`). This slice does not convert those inherited
UI paths to asynchronous validation or claim they are RAM-only. Every new
recovery entry and control returns while activation is OFF; a future production
factory must retain the worker boundary without copying these reads into its
GUI callbacks.

Focused generated-TL/core/store fixtures use injected transports only. Literal
OFF control bodies and the queued controller start run on QCoreApplication's
main thread; the real store and Carry flow run on the worker. These checks do
not qualify Telegram, native GUI lifecycle or physical power-loss behavior.
The full host run belongs to the coordinator's final landing batch.

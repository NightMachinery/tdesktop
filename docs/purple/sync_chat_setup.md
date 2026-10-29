# Sync-chat setup on Desktop

The per-account Sync chat row opens a status and setup box. Its snapshot is tied
to the opening account, login, root, Work Mode, app-wide source, binding and
resolution run. Every callback and confirmation checks that authority. Background
resolution never opens a dialog.

A complete Unset result, an exact empty inventory, and no settings binding,
effective cache or carry are required for fresh setup. Existing and bound
streams remain read-only. The fresh chooser offers a private Purple channel,
Saved Messages and up to fifty currently loaded chats, including archived ones.
Full info is read before a row is enabled and again before acceptance. One
DesktopSyncChatSetupEligibility predicate delegates to the same core eligibility
used for mark acceptance and posting. Unknown rights, admins or readers refuse.
The confirmation rechecks the disclosed reader snapshot, including available
member digests. Changes require a fresh confirmation. No account roster is synced.

Reader disclosure uses account-settings r3 for every account: settings can name
chats and people from other accounts added to Work Mode lists. All readers can see Telegram cloud sync data,
which is not end-to-end encrypted, playlist sources and the Purple mark. The C6
persistent warning is shown without blocking posting under prior consent. The
current reader counts, pre-history distinction and unavailable-list blind spot
are stated. Refused marks name the chat and the core refusal reason.

Setup confirmation starts the per-account, journaled setup service. It supports
private Purple channel creation, Saved Messages and eligible existing chats, with
explicit retry, Saved Messages after a definite channel limit, same-target reader
reconsent and cancellation. Existing/bound streams are not moved. Explicit manual settings publication uses the selected typed chat and a
worker-owned publisher, described in [Manual settings publication](#manual-settings-publication).
Automatic sync remains Self-only.
Source and host tests do not authorize native writes. Native qualification still
requires the user's explicit GO.

## D-M2B4-8

An exact zero-row response is Unknown. Peerless messageEmpty entries in history
or inventory are Unknown and hold the inventory. Only an R3 reread of the cached
mark may classify a same-id peerless messageEmpty as MarkGone, with independently
readable chat evidence. Wrong ids, contradictory peers and unreadable chats are
Unknown. Record exact reads require an explicit matching peer on messageEmpty;
peerless and zero-row record replies are Unknown, including legacy Self. The
cached-mark R3 exception does not apply to records. The model now checks zero rows defensively too; the service already
refuses them. The genuine TL fixture exercises these boundaries across Self,
ordinary chats and channel routes. Native N3 must still record whether Telegram
supplies peer_id for a deleted Self or channel mark.

All implementation and registration is in Purple-owned paths; no upstream hook
or Copy-as-Markdown path is changed. The existing Sync setup box only adds a
per-account Sync chat row and an opening-account fenced child box.

## Settings disclosure in this build

This Sync chat dialog is opened only from the per-account settings Sync screen;
there is no playlist-only setup entry point yet. Both existing-chat and Create
confirmations therefore show the settings reader warning and amendment-13
cloud disclosure for every account, including when setup is still held or
settings posting is off. The text is not conditioned on settings ownership.

Actual automatic settings sync is on when the account's private SyncAutoRecord
has enabled=true (AutomaticSettingsSyncEnabled). A modern per-account settings
store also supports explicit manual settings operations; its initialized binding
is not an automatic-sync switch. The legacy file-local sync.sendAfterSave option
belongs to the legacy send-after-save route. None of those switches hides this
setup disclosure, so an enabled account can never miss it. The confirmation names
the chosen chat and states that state.toml stays on this device.

## Journaled setup service

The Purple-owned executor exposes Next, Complete and effect-specific Renew, with
a durable save callback. The UI calls a service whose serial worker owns the
executor, journal, account lock and cache writes. A separate foreground transport
owns the shared transient request sender and native API callbacks.
Genuine TL builders prepare private broadcast creation, a silent canonical mark,
a silent pin, origin-account mute_until INT_MAX and folder 1 archive requests.
Tests inspect those requests; they never send them. Limit, restricted and flood
responses remain typed. Try again is explicit, resolves again, has a three-create
budget and honors a persisted flood deadline. Saved Messages fallback is allowed
after a definite rejection; an unresolved ambiguous creation stays held.

The journal is per account, private and worker-only. It takes the same account
lock as other modern sync writers. Admission requires a domain-separated digest
of the persisted login authority, account, root, Work Mode, home account and
binding. RAM origin and request tickets separately fence each callback. A later
native resolver must borrow this account lock through DesktopSyncChatOperation;
it must never independently take a second writer lock. Journal fields are
mandatory, typed and canonical, including the operation UUID and stable mark
random id. Missing, extra, duplicate, newer or malformed data refuses without
overwriting or deleting it. An existing operation cannot be replaced by a new
one. A changed login or context refuses the old journal.

Every write intent is saved before a command is issued. After a crash, an
uncertain createChannel only emits ReconcileCreate. A title match, a lone loaded
channel, a search absence or an RPC timeout cannot justify another creation.
Only retained Updates tied to the exact operation's create request can identify
its result; without that evidence reconciliation holds. The complete Updates
receipt is saved before ApplyUpdates; the latter must finish normal upstream
Updates handling before PeerInfo or a mark can run. Unknown eligibility, admin
counts or readers refuses. A separate complete exact inventory of the chosen
target must be Empty before the first mark, including on a not-yet-issued
post's explicit resume. Present or Unknown is held. A second fresh full-info and
reader check follows that inventory before the unissued mark. The actual typed
scanner, inventory finisher and production inventory-reply mapper preserve that
evidence. A changed initial reader disclosure holds with a fresh consent offer.
Only an explicit acceptance for the same account, operation, ticket and unchanged
destination resumes it, and that resume repeats resolution, inventory and full
info. Aborting or refusing the offer writes no consent. Pending mark intent is
never converted into an unissued post by reconsent.

A resumed Pin, Mute or Archive with a sticky new-reader warning first issues
an explicit cache Commit, retaining its exact decoration resume point in the
journal. A failed cache write holds. The original baseline and warning survive
subsequent departures and another restart.

The mandatory journal contains the original reader snapshot as well as its
digest. After a mark has been issued, known new readers retain prior consent and
set a sticky C6 warning. Departures alone do not set it; unknown readers and lost
rights still hold. The cache uses the original accepted counts and member
digests across restarts, so the new population cannot silently become the
accepted baseline. The snapshot and warning are local only, never synced as a
roster.

The mark random id is saved before sending. Missing or ambiguous acknowledgements
reconcile that same random id. The whole exact TL readback response is retained;
inexact, flood-limited, zero-row, duplicate-row and contradictory-count responses
hold. A valid response then requires an exact canonical owned text, id,
date, typed peer and no media. Cache commit follows that reread. Only a newly
created channel then advances through silent pin, mute and archive. Existing
chats and Saved Messages finish after the mark/cache step and are never pinned,
muted or archived by setup. These steps are idempotent and are recorded before
and after each effect. Restart/resume first resolves again and revalidates full
info and an already known mark. A bound stream, Follow/Fill/carry or an uncertain
inventory stays held. A newly resolved stream may be adopted before any own
created/marked work; it is never overwritten.

One worker owns all executor methods and the journal store. The foreground owner
revokes an atomic capability immediately and queues cancellation to that worker;
no GUI thread enters file IO or waits for an executor callback.
The foreground owner supplies an atomic current capability revoked on logout,
login reuse, source or binding change, account switch and close. The GUI never
waits for this executor or file adapter. Cancel drops the in-flight ticket,
keeps uncertain intent bytes and starts no cleanup. Terminal cancellation releases
the journal store and account lock on the worker queue even while the box remains
open. It does not regain admission to write a cancellation marker. A stale completion cannot cancel
a newer ticket. Store failure visibly holds and preserves the last durable
checkpoint. No step runs on a background discovery timer. The UI displays running, held, channel-limit, cancelled and complete states.
Resume opens only an existing journal. A missing journal completes the lookup
without creating an intent and releases the empty store and worker. The chooser
then clears the service on the next GUI turn, shows a clear no-pending-setup
message and enables fresh Continue and Check again. A generation fence prevents
a late empty result from resetting a newer setup. Closing the box
revokes admission immediately. Native outward testing remains behind explicit GO.

Android A3 shares these states, journal/ambiguity rules, request flags and reader
consent boundaries. Platform details differ: Desktop uses a QLockFile/private
atomic-file adapter and cancelable shared-sender full-info reader; Android uses its
account-specific storage and request owner. D6/D7/C6 policy already landed on
Desktop; Android delivers its alignment separately. Neither source delivery is
native or outward qualification.

The setup journal is durable before any transport command and is loaded before
any new operation can begin. Reopening requires explicit Resume. Its only
creation reconciliation evidence is the original request's retained Updates and
operation UUID. If that receipt was lost across restart, setup remains held;
there is no title-based adoption or blind create retry. Saved Messages and
existing-chat setup never pin, mute or archive an unrelated chat.

A definite deferred write goes through the same shared resumeEffect path as
other sync writes. Before replaying the captured request, the service renews R3
and full-info evidence. An unissued mark additionally repeats exact complete
Empty inventory and a second full-info reader check. Decorations re-read the
exact canonical owned mark and publish any C6 warning to the cache first. The
worker replays these proofs through the real executor before releasing the gate.
Chat, random id, mark id/date, operation and ticket cannot change. The original
TL request is reused, so retry does not mint a new send identity. Ambiguous
server/transport/timeout outcomes enter reconciliation or a held state instead.
The shared transport wait is in memory, while the pending effect intent is
durable. Restart uses journal reconciliation; it does not reset uncertainty.

Tests compile the real flow, journal, executor, request builders and cache adapter.
A second fixture includes literal production renewal callback bodies, with only
session, before-post, metadata, inventory, API queue and worker delivery endpoints
substituted. It checks cancellation and superseded callbacks as well as all five
write kinds. These host fixtures never contact Telegram. The source route gates
also enforce worker ownership and explicit manual settings authorization.

The exact setup mark uses the shared canonical settings/playlists and deletion
explanation, followed by #purplechatsync v1. Discovery's grammar is unchanged.

## Input lifetime

PeerData::input(), ChannelData::inputChannel() and UserData::inputUser() return
owning TL values. Retain that value in a named local before keeping a reference
to c_*() or data(). A reference returned by an accessor on a temporary does not
extend the temporary's lifetime. Both full-info and cached-mark channel reads
retain the input while copying the channel id and access hash into their request.
The existing purple/test_sync_chat_setup_routes.py source gate scans all Purple
C++ sources and tests unsafe and safe controls. Reaching these exact session-owned
request paths in a standalone unit would require the native account/data stack.

## Temporary Telegram failures

Full-info, discovery, inventory and shared sync write requests keep temporary
Telegram failures pending with a visible retry time. See
[sync request retries](sync_request_retries.md) for deadlines, cancellation,
the host-only verification boundary and the in-memory state limitation.

## Manual settings publication

The settings screen checks the resolved typed destination for the selected
account. Its inventory, settings review, destination caption and explicit send
confirmation remain tied to that account and login. Reader consent still comes
from the setup disclosure. The settings confirmation repeats the account-settings
disclosure, including IDs and names added from other accounts. Known new readers
keep the persistent warning and prior consent; unknown readers or lost posting
rights hold. Setup completion alone never sends settings. Automatic sync remains
Self-only.

A fresh non-Self destination initializes a version-2 ledger only after its exact,
complete inventory is empty. Self retains the version-1 initializer and document
format. An existing ledger must match its typed chat and stream. Choosing another
mark does not move or overwrite it. Carry and migration remain separate work.
Joining without a local settings file initializes on the worker and sends nothing.

The first settings record uses the shared canonical config builder, filename,
MIME and #purplesync caption. The durable pending record precedes upload. There
are no pin, mute or archive effects for this document. Confirmation requires the
same exact owned typed message and bytes after fresh metadata and download checks.
An uncertain result keeps pending evidence for Finish sending; an exact matching
record found after restart confirms locally instead of posting another copy.

SyncConfigPublish owns a serial disk worker. The GUI captures only cached routing
and account-local preference values. A detached routing snapshot validates the
owner, local choice, directories, settings bytes and store on the worker; it never
refreshes the live router. Open, recovery discovery, initialization, staging,
confirmation and lock release stay on that queue. The publisher’s typed inventory and readback callbacks use the captured RAM routing
fence rather than rereading files. A fresh
before-post R3 and full-info check is followed by worker authorization before each
effect, including definite flood retries. Immutable staged bytes, file identity
and random identity remain in the upload operation.

Only an exact matching Complete setup journal is admitted. Other, unfinished or
malformed journals hold without deletion. When this same verified initialization
creates a binding token, the worker rebinds the already verified journal under the
account lock, after an exact-byte comparison. No fallback accepts an unbound or
mismatched journal. A crash or write failure across that boundary holds for review.
The account preference write uses the existing cached preference API and its
scheduled persistence; a missing token on restart follows binding recovery.

Close, account or source changes, logout, passcode lock and application inactivity
revoke RAM admission immediately, cancel transports and retire the worker/store
without a GUI wait. Staged pending bytes survive. A superseded completion cannot
finish a newer publisher. Temporary Telegram waits remain one logical operation
with their retry time visible; cancellation converts them to durable pending
reconciliation on the next explicit operation.

The production worker and literal upload-gate host fixture is
purple/test_sync_config_publish.cpp. It uses real private files, the real ledger,
recovery and config planners, with injected transport endpoints. It never calls
Telegram. Existing review/import/History adapters retain their earlier UI-side
inspection work; this change removes disk work from the publisher and its activated
transport callback path, rather than refactoring those independent operations.

### Cancellation and settings-source changes

The RAM routing snapshot includes both routing and settings-source generations.
Every settings invalidation advances the source generation, including a reload
whose bytes are later restored. The GUI settings-change subscription cancels the
publisher immediately; every activated transport callback also checks the same
snapshot. Disk authorization remains a second check on the worker. A long wait
cannot revive an operation after its reviewed source changed.

A terminal publisher marks itself done and revokes admission before cancelling
its child post. It queues worker retirement before delivering the single terminal
callback, which may destroy the publisher. Pending stage bytes remain available
for an explicitly reviewed retry. The real-file worker and literal coordinator
fixtures cover source changes during a long wait, change then restore, account
lock release on queued retirement, and destruction from the completion callback.


## Change sync chat (qualification held)

The bound account has a distinct **Change sync chat** entry. Fresh setup remains
separate and still refuses a bound settings stream. The move chooser captures
its owning window and account/login, exact state bytes already retained by the
discovery worker, the admitted cache, the source chat and current winner. There
is no additional GUI-thread disk read. A changed source, state, cache, account,
login, winner, destination eligibility or reader disclosure refuses the old
confirmation. Foreground/window loss, passcode lock and closing revoke it.
Dismissing a confirmation grants nothing and allows another explicit choice.

The confirmation shows the destination's current reader facts, the settings
file disclosure (including Work Mode list identities) and cloud visibility.
The old chat itself stays. The optional cleanup covers only Purple's own
settings messages and mark after commitment, never unrelated content or the
whole chat. It starts unticked when the old settings chat is Saved Messages.
The exact checkbox value belongs to that confirmation, not a global preference.
Cleanup remains subject to the Carry executor's separate authorization and its
currently held deletion adapter.

Native move activation is off. The service validates the final RAM snapshot and
then reports qualification-held. It creates neither a provisioning worker nor
a Carry worker, transport, durable intent or background permission. Reopening,
Retry and closing cannot enable dispatch. The production activation predicate
is pinned off by host tests; a future owner-approved activation must supply the
reviewed executor integration rather than changing the checkbox into consent
for arbitrary background effects.

### Provision-only creation

A purpose-tagged variant of the T3 create journal prepares a new destination
without selecting it. It is bound to the captured source fingerprint, including
exact settings state and cache. Its explicit consent precedes the durable create
intent. It reuses T3's operation identity, at-most-three definite create attempts,
matched-request receipt and retained ambiguous-create proof. Restart with an
uncertain create reconciles or holds; a matching title is never adoption proof,
and another channel is never created just because the first response was lost.

The sequence is fresh resolution, create/reconcile, apply the owned Updates,
full info, complete exact Empty inventory, then full info again. It finishes as
**provisioned**, with no mark and no admitted cache. It cannot issue PostMark,
Commit, Pin, Mute or Archive. Nothing silently deletes the channel after failure
or cancellation. A provisioned channel is not a committed move. Carry owns the
records and mark, and only its verified commitment can precede decoration.
The existing T3 silent pin/mute/archive policy is for a newly created channel
only; existing chats and Saved Messages must never be decorated. All move
provisioning and subsequent decoration remain held with native activation off.

Fresh setup journals keep version 1. Provision-only journals use version 2 with
mandatory `purpose: "provision-move"` and a 64-character lowercase SHA256
`source_fingerprint`. The bounded canonical codec refuses missing, extra,
unknown-purpose and newer-version fields without replacing the file. A fresh
setup flow cannot resume a provision journal, or vice versa. The shared core
state and Carry journal formats are unchanged.

## Remove one older mark

The dormant selected-mark action lists supported Own marks observed for this
account when a complete resolution has a newer winner. It is separate from
moving or cleaning up settings. Production activation remains constant OFF:
the real screen exposes no clickable Remove mark row or confirmation, and the
public entry refuses before dispatch. The host injection seam tests the action;
this does not qualify a native UI or Telegram deletion.

A row captures the account/login, typed chat, exact message id, date, full text
and author identity. Search only proposes rows. The worker serializes with the
account's sync operations, holds its local store lock, resolves the winner
afresh, checks actual active and retained Carry journals, and rereads that
single message with fresh independently readable peer information.
`CheckStaleMarkRemoval` is the sole positive authority. A current winner,
foreign/newer mark, changed date/text/author/chat, incomplete resolution or
uncertain read holds. No Switch journal or old-mark cleanup authority is
manufactured. Active or unreadable journals hold; retained unresolved operations
protect their typed-chat/message-id references (even if a date changed) and attempted mark text. Validated Done history does
not permanently block the account.

The single-use RAM permit authorizes only the captured id in the captured
ordinary or channel namespace. The shared request controller's effect-resume
gate repeats resolution, journal checks and exact authorization before a
definite retry. It retains the original request bytes and refuses a changed
typed route. Ambiguous outcomes hold for an explicit new action and reread.
The native port starts transport on its sender thread and explicitly marshals
worker renewal back to that thread before resuming the shared request controller.
Captured-account affected-message counters must be applied successfully before
the confirmation reread. An acknowledgement alone never reports removal.
Close/logout/window or account departure revokes the RAM capability immediately,
then worker cleanup releases only this operation's store and account lease.

D-M2B5-12 accepts peerless MessageEmpty only for an exact single-id read of the
selected stale Own mark in its captured typed chat, channel-scoped for a
channel, with the same id and independently established current readability.
An explicit peer must match. Contradicting/dangling peers, zero or duplicate
rows, other ids, inexact/flooded responses, changed ordinary messages and other
uncertain shapes remain Unknown. This exception only concludes AlreadyAbsent
or confirms absence after applied affected-message counters. It never grants
deletion authority. Record and history/inventory reads retain their conservative
rule: peerless record MessageEmpty is Unknown. This action cannot delete
settings records, other messages or an entire chat.

Host fixtures compile the actual core/TL adapters and worker controller, and
port the literal shared-controller transport body with a synthetic sender.
They do not execute a Telegram request. Full canonical qualification is deferred
to the final landing batch by the owner's velocity rule.

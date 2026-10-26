# Telegram request retries

Purple's sync read and shared write handlers use `SyncRequestSender`, a client
adapter around `MTP::Sender`. Temporary read failures retain the current request.
A definite flood refusal retains a write intent; an ambiguous write outcome
returns through the existing outcome-unknown reconciliation path. It does not complete full info with null, turn a readable chat
into Gone, discard a discovery cache, or publish an empty inventory. Permanent
errors continue through the existing failure handlers.

## Timing and ownership

Flood waits and premium flood waits of at most 30 seconds get one short retry
when the server wait fits inside the remaining 30 second request deadline. The
adapter waits X+1 seconds and extends that deadline by exactly the wait. The
short retry budget belongs to this request, including later deferred attempts.

A longer flood, a repeated flood or a flood outside the remaining deadline
produces a `SyncRequestTransient` with `retryAt`. The owning client controller
schedules continuation at the deadline. Read and file-part server/internal
errors, negative transport errors and the adapter's own timeout use 5, 10, 20,
40, then 60 second backoff. A send, edit or delete with one of those ambiguous
outcomes is never retransmitted by this controller. Its original owner
reconciles the durable intent. Malformed flood durations wait conservatively; oversized durations
saturate rather than overflow. Long timers are armed in bounded chunks and never
send early. A deferred attempt starts a fresh 30 second response deadline.

Each sequential request stream has one pending request and one timer. The same
owning TL request is copied for retransmission, preserving peer, access hash,
file id, part, edit target, bytes and send random id. It does not call the
high-level post/create executor again or allocate another message random id.
Every dispatch and completion checks its captured account/session/routing
predicate. Cancellation removes the timer and wire request. Weak ownership,
an active flag and an attempt generation reject retired callbacks. Predicates
may retire their owner, so lifetime is checked again after evaluating them.

Full-info's old outer null timeout is removed. Discovery's aggregate network
timeout yields to the request controller, including nested inventory requests.
Candidate downloads have a separate 30 second response watch. A stalled loader
is retired immediately, with its generation revoked. A typed timeout notice
waits five seconds before fresh metadata and a new loader for the same candidate.
Old loader callbacks cannot complete the candidate or an empty inventory.

The initial local-open timeout has a separate five second scheduled retry. Its
logical callbacks, borrowed account operation and before-post intent survive
that wait. An Ensure call joins that pending operation even at or after its
retryAt deadline; it never replaces the borrowed lock capability with a new
argument. The original timer resumes it. UI
publication does not take completion callbacks. A late worker is rejected by
its old token, including while an owned lock is still being released. Account,
login, binding or source changes refuse continuation. Force
requests merge into an active stream or its wait rather than creating another
retry. No discovery cache is written while Telegram requests remain pending.

## Visible state and shared core

Pending retries expose a typed transient outcome and absolute retry time through
`PendingSyncRequest` and `SyncRequestChanges`. The chooser and automatic sync
status display that Telegram asked to wait, or is temporarily unavailable, with
the local retry time. These presentation notifications do not emit sync policy
triggers. Successful recovery clears the pending notice. Posting remains gated
by the in-flight command and its origin, consent and lease checks. Every
definite write retry first renews the before-post R3/full-info facts. Edits and
deletes also exact-read and authorize the target again. A missing mark, changed
remote bytes or ownership, unknown readers, or lost rights refuses the replay.
The owning TL request remains unchanged, including its original file, part,
random id and target. The response clock is paused while the asynchronous
authority gate runs because no effect request is in flight; the short-flood
retry budget is not reset.

No core source or schema changes are needed for this in-memory continuation:
read continuations retain their completion rather than turning a transient
into final UnknownFacts or null. Ambiguous writes deliberately use the existing
outcome-unknown path, which requires reconciliation before another effect. Existing logical automatic-run retry counters are
not reset. The controller holds the operation pending while backing off; logout,
Disable, cancellation and origin retirement still cancel it. A later project
that needs durable transport retry deadlines and release of the operation lock
while waiting would need a distinct core transient completion carrying retryAt,
plus persisted pending-request identity and resume authority. It must not reuse
Paused/UnknownFacts for that purpose. This patch does not add that durable state.

## Verification

Run `python3 purple/test_sync_retry.py --output-dir <private-scratch>`. The host
fixture uses the production retry state and extracts the literal production
`dispatch`, `failed`, `tick`, `resume` and `cancel` bodies. A second program
uses the literal templated wire callbacks, effect renewal, delete gate,
local-open retry/publication and download watch/retirement/restart bodies. An injected clock, timer and
transport replace the event loop and Telegram. It covers both flood forms,
short and long waits, one-short-retry budget, deadline boundaries, timeouts,
bounded server/transport backoff, unchanged permanent mappings, cancellation,
reentrant origin retirement and immutable payload identity. A source gate
inventories all 15 call sites and checks the common controller and timeout
wiring, including negative mutations for effect-specific continuation. The
integration endpoints substitute native leases, fresh-info replies, target
reads, disk workers and loaders; they do not exercise Telegram or native file
loading. The actual typed request templates are syntax-checked with the existing
Debug compile flags. This is host/source qualification, not a native round.

Native qualification after review must use the product handler, keep the same
private test profile and shared fixtures, and pace the full-info sequence. A
server wait is handled by the product; the test harness must not blindly restart
the scenario or request a fresh login. Native calls and fixture lifecycle remain
separately authorized.

## Read queues with their own cooldown

SyncRequestSender can hand a non-short transient to an optional owner callback.
The controller retires the logical request, timer and wire generation before
calling that owner with the same request id and transient retryAt. Short floods
stay inside the controller for their single inline retry. Owners without this
callback keep the existing pending/reschedule behavior. Ambiguous write effects
still take the outcome-unknown path before any handoff.

The emoji exporter uses this read-only handoff to feed its existing automatic
cooldown scheduler. Transients hold metadata resolution globally, while cached
artwork may continue; permanent document failures keep their own cooldown. Its
outer request timeout is removed so it cannot turn an extended short wait into
a document failure. Media loading retains its separate timeout. The complete
Purple inventory now covers sixteen managed request sites.

Manual settings publication keeps its worker-owned store and RAM capability
through a transient request. Every resumed definite send first renews R3/full-info
and then asks that worker to validate the unchanged source and pending store.
No staged file or routing target is reread on the GUI thread. Edits retain their
additional exact owned-target gate. Cancellation revokes the capability and
releases the worker/store while preserving pending reconciliation evidence.

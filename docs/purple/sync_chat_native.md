# Native sync-chat requests

The Desktop native adapter connects the carry executor and stale-mark controller
to Telegram requests. All five feature switches remain off. Constructing the
adapter sends nothing; foreground service and startup integration are separate.

## Constructing the adapter

Include `purple/purple_sync_chat_native.h`. On the GUI thread, call
`MakeDesktopSyncCarryTransport(session, current, context)`, where `context`
contains an exact `DesktopSyncChatSetupSnapshot opening` and
`DesktopSyncNativeRenewal renewal`. Bind its `effect`, `beginCleanup` and
`cleanup` callbacks to the originating engine's `renew`, `beginCleanupRenew`
and `renewCleanup` methods. Those callbacks enter on GUI, post the method to
that engine's serialized worker, then return `ready` on GUI.

The returned closures keep the adapter and request owners alive. Call `send`,
`cancel`, `applyUpdates` and `applyCleanup` on GUI, and release the closures on
GUI. The outer owner still owns the worker, store, gate, window and lifecycle.
It must make `current()` permanently false when its account/window authority
is revoked. No reply or new send follows revocation. Known Updates still reach
the originating live session before local completion is suppressed.

New operations must supply the already accepted destination peer to the existing
`engine.open(start, acceptedPeer, stream, openingImages)` call. This persists
the reader baseline before native transport replies. The adapter never invents
reader consent from current membership. Resumed operations use their journal's
persisted baseline.

For startup, build `opening` on the worker from the validated state and cache:
account/login/binding/root/owner/mode from the captured carry scope,
space/settingsBound/settingsChat from state, and mark/carry from cache. Preserve
exact state/cache bytes. Initial `status.complete=false`, `run=0` and default
remaining input are sufficient. A fresh read-only resolver rebuilds remote facts.
Its private model view omits the cache's carry reference; the original cache and
journal remain untouched, and that view supplies no posting authority.

`MakeDesktopSyncMarkNativePort(session, current, opening)` supplies the mark
controller's `exact`, `send`, `apply` and `cancel` fields on the same request
plumbing. The caller supplies origin/login/window/current/foreground/worker,
fresh resolve and status callbacks. These four native fields enter on GUI.
The mark controller owns authorization, the account lease and absence checks.

## Commands and wire effects

Every accepted reply preserves the command's ticket, run, operation and revision.
Only one command runs per carry adapter. An overlapping or malformed command is
Unknown and cannot replace the active one. Local journal actions do not become
network requests.

- ResolveAgain runs the existing discovery model using paged global/Self/dialog
  search, typed exact mark reads, fresh full info and history evidence. It does
  not open or save the ordinary discovery store. Searching uses the model's
  bounded retry schedule before Lost; unknown results remain held.
- Plan and effect preparation collect fresh source/destination full info and
  complete typed inventories through `SyncAccountInventory`. Partial scans,
  foreign routes and invalid receipts are Unknown. Protected sources supply
  only the scope of facts allowed by the existing core; an unknown error is
  never translated to ChatGone.
- ResolvePending returns the executor's persisted send-intent evidence, exact
  source inventory and a bounded worker read of this account's settings text.
  Only the core decides whether to finish, defer or abandon the stage.
- Deliver dispatches only issued permits. Forward requests retain captions,
  suppress attribution and are silent. Own and pending records use
  `SyncConfigUpload` with the permit's persisted file/random IDs. The executor
  receives the original native Updates response, not a synthesized receipt.
- PostMark uses the permit's persisted random ID and exact mark text in a silent
  `messages.sendMessage`. Fresh renewal is required before dispatch and on the
  shared request controller's retry; renewed wire bytes must remain identical.
- VerifyDestination obtains a new complete target inventory. ReadMark uses
  `channels.getMessages` for channels and `messages.getMessages` otherwise,
  preserving typed chat identity and the exact returned message.
- PrepareCache and CompleteCache supply fresh remote facts; the executor retains
  exclusive authority for local persistence.
- Cleanup obtains fresh destination coverage and exact old record/mark reads.
  It re-seals record bytes against the exact response. Permitted deletion uses
  `SendDesktopSyncChatCleanup`, including its renewal and timeout fences, and
  selects channel versus ordinary exact-ID deletion. No whole-chat deletion is
  available. The executor records uncertainty and proves absence on a later read.

All native requests use the shared `SyncRequestSender`. Uploads apply their own
Updates; other effects apply Updates through the originating session before the
reply reaches the executor. Cleanup applies affectedMessages on the typed peer.
The returned carry `applyUpdates`/`applyCleanup` callbacks acknowledge this work,
so the outer service must use those callbacks rather than applying again.
Mark removal applies affectedMessages before checking revocation, then its
separate `apply` callback consumes the matching typed receipt once.

The native backend's GUI work is limited to request/session operations. The
bounded settings-file read runs on a worker. Facts and permits passed to the
executor carry an atomic admission predicate, so worker checks do not inspect
GUI objects. Cancellation retires only this adapter's requests and read jobs.

## Winning-mark commitment

ReadMark accepts a live mark only when fresh resolution passes the executor's
scope and reader checks, and its exact winning chat, message id and date match
the read mark. This applies to uninterrupted and restarted operations. A newer
same-chat or different-chat winner holds the existing operation before state or
cache commitment and before old-source cleanup. Automatic recovery from that
hold is a separate slice (D-M2B5-13).

## Host verification

`test_sync_chat_native` drives the actual resolver and dispatcher using injected
native replies and the real worker executor. `test_sync_chat_native_routes`
checks native wiring guards and all five OFF switches. The backend is compiled
with the test; it is never given a real session. These checks do not qualify a
native account, server timing, installation or activation.

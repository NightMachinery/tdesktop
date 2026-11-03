# Magic Purple Sync: design

The design of the magic sync chat, written before the R4b first-use policy. Where they differ,
[sync-magic-r4b.md](sync-magic-r4b.md) wins. "D1" is the owner's test account and device pair.

## 1. Why the old design flooded

Every resolution ran full discovery: up to 20 `messages.searchGlobal "#purplechatsync"` pages, a broadcasts-only retry,
up to 50 per-chat `messages.search`, a Saved Messages search and a `#purplesync` hint search. Before-post checks forced
it again unless a RAM receipt younger than 15 min existed; Searching retried at 30/120/600 s; the transient-wait
deadline lived only in RAM, so each restart searched again. Separately, both clients run a launch-time Saved Messages
document search for the old settings offer (Android `PurpleSyncOffer.checkOnLaunch` :224, Desktop
`SearchForSettingsOffer` purple_sync.cpp:704-744). The new design issues no search request of any kind.

## 2. Terms

- **Magic chat**: a megagroup whose title is exactly `Magic Purple Sync`.
- **Eligible magic chat**: a magic chat where the user is creator, the only member and the only admin; no public or
  active secondary username; not a forum, gigagroup or migrated chat; and passing the existing core refusals in
  `SyncChatEligibility` (TTL, protected content, linked chat, paid messages, cannot send files, anonymous/send-as).
- **Binding**: this device's record, per account, of which chat is the sync chat. It is stored in a new file
  `sync/chat.json` (codec in core) and mirrored into `state.json` `chat` (state v2). Mode is `magic` or `manual`.
- **Lookup**: the one routine that turns the binding plus the local dialog list into Use, Wait, Create or Problem.
- **Wait**: a transient condition (flood wait, network, timeout, list still loading). Wait never pauses automatic
  sync and never shows a dialog; it only defers. Only a FLOOD_WAIT, FLOOD_PREMIUM_WAIT or SLOWMODE_WAIT is worded
  as Telegram asking Purple to wait (R2).
- **Problem**: a real condition (chat gone, rights lost, another member, foreign or forwarded record, creation limit,
  a lookup request Telegram refused with a 400-class error: `request_refused`).
  A Problem is shown once and pauses automatic sync until the user acts.

## 3. Behaviour

**Lookup** (single-flight per account; triggers: startup, screen, manual, auto, post; every trigger runs the same
code and only the trigger label differs in diagnostics).

1. Manual binding: the chat must still be in the local peer cache and joined (not left, kicked or deleted). Use it.
   Never create, never consult the magic title.
2. Magic binding: the bound chat must still be present, joined and eligible by local data. If an *older* eligible magic
   chat is also visible, rebind to it (convergence rule, see 3.4). Otherwise Use. A rename does not unbind.
3. No binding: collect magic chats from the local lists, archive (folder 1) first.
   - An eligible match in the archive is enough: no main-list load is needed.
   - Otherwise decide "absent" only when both lists are fully loaded (Android `isServerDialogsEndReached(0/1)`;
     Desktop `chatsListLoaded(nullptr)` and `chatsListLoaded(folder(1))`). Desktop must request the archive once
     (`api().requestDialogs(folder(1))`, it is not loaded unless archive filters need it) and wait for
     `chatsListLoadedEvents()`. Android asks upstream `loadDialogs` for the next page of the incomplete folder, one
     page at a time, never in a loop faster than upstream itself. A partial list is Wait, never Create.
   - Several eligible: pick the lowest channel id (ids are allocated increasingly, so lowest = oldest). Every device
     makes the same choice.
   - Magic chats exist but none is eligible: Problem `magic_ineligible` (no second chat is created behind the user's back).
   - None at all, lists complete: Create.
4. First use on this device (and each new login): one `channels.getFullChannel` confirms creator, member count 1, admin
   count 1, no usernames, before the binding is written. Re-verified on manual Check and at most once per 24 h before
   an automatic post (timestamp kept in the binding). Every other check is local.

**Create**: `channels.createChannel` with `megagroup=true`, title `Magic Purple Sync`, short about text; then
`folders.editPeerFolders` to folder 1; then `account.updateNotifySettings` with `mute_until=INT_MAX`. No mark message,
no pin. Each step is journaled in `sync/chat.json` before dispatch. An uncertain create (timeout, lost reply) records
`create_uncertain_at` and blocks another create for 24 h; the next lookups find the chat from the dialog list once its
Updates arrive. `CHANNELS_TOO_MUCH` and similar limits are a Problem. Archive or mute failures that are transient retry
under the Wait rules; permanent ones are logged and do not block sync (they are cosmetic).

**Convergence (3.4)**: two devices may both see "absent" and both create. The deterministic oldest pick makes each
magic-mode device rebind to the oldest eligible chat when it becomes visible. Rebinding runs the same migration path as
section 4, so a device that published into the younger duplicate republishes into the oldest. The younger duplicate is
left untouched (no deletions); the screen shows "An extra Magic Purple Sync chat exists and is unused".

**Records**: only messages that pass core C0 ownership as Own in the supergroup path are records: sender is the user,
not forwarded, not via bot (incl. business/guest bots), not scheduled, no TTL, regular message
(`GetSyncChatMessageOwnership`, purple_sync_chat.cpp:86-143; already the rule, so no new predicate is needed). A
record-looking message (core `IsSyncHistoryCandidate`) that is not Own is never applied and raises Problem
`foreign_record`. Service messages and the user's own non-record messages are ignored silently. Record format,
inventory scan, publish/apply, conflicts, History/Undo and the automatic policy are unchanged.

**Hidden in Purple**: the bound chat id of each account (magic mode only) is hidden from Purple's chat lists (main,
archive, folders, Work Mode views) by a new predicate, default on, with a "Show sync chat in chat list" toggle on the
Sync chat screen and an "Open sync chat" button there. It stays reachable from search and direct links.
Android: `PurpleFolders.hiddenFromLists` and the `MessagesController` filter hook (:22477, :22519), plus folder 1.
Desktop: `History::purpleHiddenFromView` (purple_history.cpp:113-150) extended to apply with no preset running.

**Transient deadlines**: `sync/chat.json` keeps `waits`, a map from Telegram method name to a wall-clock `retryAt`
(the server's FLOOD_WAIT seconds, or the existing 5/10/20/40/60 s backoff for network and 5xx). Every sync request
goes through one gate that refuses dispatch before its method's deadline, across restarts and for every trigger. A
flood of at most 30 s keeps the existing single inline retry. Upstream-owned requests (getDialogs pages) keep
upstream's own flood handling; lookup only waits for them.

**Problems and pause**: core gains `SyncAutoReason::SyncChatProblem` and a binding-level `problem{code, shown}`.
`Hold()` already deduplicates notices (`noticed`), and `ResolveSyncStatus` already ranks PausedForProblem. Show once:
Android a one-time dialog when the Sync screen or main activity is foreground, else a status-row badge; Desktop a
one-time toast linking to the Sync chat box. Resume = user acts (Check, Resume, choose chat, create new). Transient
conditions never set `problem`.

**Manual override**: the Sync chat screen keeps a picker over the local dialog list (no search), limited to Saved
Messages and chats where the user is the only member (see Q2). The choice is stored per device as mode `manual`.
Returning to default clears it and runs lookup.

## 4. Migration ("move over auto")

On the first successful lookup on a device whose state is v1 Self or bound elsewhere:

1. If an old pending stage exists for Saved Messages, drop it locally. Its outcome no longer matters because
   Saved Messages is no longer read. The bytes are kept in the existing archive dir for History.
2. Core `MoveSyncChat(state, newChat)` sets v2 with the chat and clears the own-message ledger (message ids belonged to
   Saved Messages).
3. Read the new chat's inventory (ordinary `getHistory` scan). Empty: publish current local settings with the local head
   as parent. Same space with records (another device migrated first): ordinary check, so apply, conflict or no-op
   as today. Different space: Problem `space_mismatch` with the existing Join action (see Q4).
4. Saved Messages records are never read, edited or deleted again; the launch offer search is removed.

Core check for P2: the reconcile/apply path must accept a head whose parent id is absent from the new chat's
inventory (parent lives in Saved Messages). If it does not, the first publish in a new chat marks the local head as
the base explicitly; this is a small core change, test-first.

## 5. Diagnostics (addendum)

Areas/events in the existing diag vocabulary style, fields only codes, counts and seconds (no ids, names, phones,
text). New area `sync_chat` with events:

- `lookup_start trigger=startup|screen|manual|auto|post`
- `lists main=loaded|partial archive=loaded|partial|requested`
- `lookup_result result=found|created|absent_waiting|ineligible|bound|rebound reason=<code> count=<candidates>`
- `request method=<tl method name> outcome=ok|flood|network|timeout|error seconds=<n> error=<type code>`
- `wait_honored method=<name> seconds=<remaining>`
- `auto_paused reason=<code>`, `auto_resumed reason=user_check|user_resume|chat_changed`
- `problem_shown reason=<code>`
- `migration step=drop_pending|move|inventory|publish|joined|space_mismatch outcome=ok|wait|problem`

Repeat limiter, both clients: each distinct line body (area, event, fields) may be written at most 3 times per
10 minutes; later copies are counted, not written. When the window ends or the key is evicted (LRU of 64 keys), one
`diag repeated count=<n> area=<area> event=<event>` line is written. Android: replaces the 5-second last-line check in
`PurpleDiagLog.event` (:116-131); it applies to every area, which fixes the 6k archive lines. Desktop has no
structured diag log today, only free-form `LOG(("Purple: ..."))`; add `purple_sync_diag.{h,cpp}` with the same
vocabulary check and limiter writing `Purple diag: ...` lines to log.txt. The limiter is client code because core owns
no logging (rule 8).

## 6. File-level change list

### purple-core (Telegram/ThirdParty/purple_core and TMessagesProj/jni/purple)
- New `purple/purple_sync_chat_magic.{h,cpp}`: `kMagicSyncChatTitle`; `MagicChatCandidate` facts; `CheckMagicChat` →
  Eligible or reason code; `PickMagicChat` (lowest id among eligible); `PlanSyncChatLookup(binding, lists, candidates)`
  → Use/Rebind/Wait/Create/Problem; `SyncChatBinding` JSON codec (`version, mode, chat, login, verified_at, waits,
  create{step,uncertain_at}, problem{code,shown}, migration{step}`) with unknown-field preservation like auto.json.
- `purple_sync_auto.{h,cpp}`: `Gate` drops `chat.kind != Self` (:113-116) and requires `chatResolved && chat ==
  state->chat`; add `SyncChatProblem` reason; codec accepts it.
- `purple_sync_chat.{h,cpp}`: delete `ParseSyncChatMark`, `ResolveSyncChat`, mark outranking, `Searching()`, reader
  warning parts of eligibility; keep ownership, eligibility refusals, `MoveSyncChat`, peer tokens.
- Delete `purple_sync_chat_carry{,_codec}.{h,cpp}`, `purple_sync_chat_journal.{h,cpp}` and their tests.
- Tests: new `tests/test_sync_chat_magic.cpp`; edit `test_sync_chat.cpp` (drop mark/resolution cases),
  `test_sync_auto.cpp` (non-Self gate, problem hold); `tests/run.sh` roster.
- Docs: new `docs/sync-chat-magic.md`; trim `sync-chat-core.md`, delete `sync-chat-carry.md`; README summary.

### Android (all under TMessagesProj/src/main/java/org/telegram/ unless noted; `p/` = messenger/purple/)
- New `p/PurpleSyncMagicChat.java` (lookup service, single-flight, triggers, diag), `p/PurpleSyncMagicCreate.java`
  (create/archive/mute journal steps), `p/PurpleSyncChatBinding.java` (sync/chat.json via core codec, CAS file),
  `p/PurpleSyncWaitGate.java` (persisted per-method deadlines over `PurpleRpcRetry`).
- `p/PurpleSyncChat.java`: facade keeps `startSession/ensureResolved/checkBeforePost` names, routes to the new service;
  drop MarkPort, carry and move hooks.
- `p/PurpleSyncChatFacts.java`: add megagroup candidate facts from local `TLRPC.Chat`/`ChatFull`.
- `p/PurpleSyncChatTelegram.java`, `p/PurpleSyncChatRequests.java`: remove `global()`, `search()`, `hint()`,
  `fallback()`; keep exact/info/history builders; add create/archive/mute builders (moved from SetupRequests).
- `p/PurpleSyncAuto.java`, `p/PurpleSyncRunner.java` (:890), `p/PurpleSyncTelegramClient.java` (:218, :240, :344):
  replace Self-only `selfReadable/selfPostable` with "bound chat resolved"; posts and edits go to the typed bound chat.
- `p/PurpleSyncChatManual.java`: resolve via lookup; migration steps (section 4).
- `p/PurpleSyncOffer.java`: remove the launch Saved Messages search; `ui/PurpleDialogsHooks.java:55` drops the call.
- `ui/PurpleSyncChatActivity.java`, `p/PurpleSyncChatScreen.java`: rewrite to status, problem/once, Open chat, Show in
  list toggle, Choose another chat, Use default; drop consent, Change, Follow/Fill, marks.
- `p/PurpleFolders.java`, `p/PurpleGate.java` hook, `messenger/MessagesController.java` filter sites: hide bound chat.
- `p/PurpleDiagLog.java`, `p/PurpleDiagLogStore.java`: repeat limiter, `sync_chat` vocabulary.
- `ui/PurpleSyncText.java`, `res/values/strings_purple*.xml`: new strings, remove dead ones.
- Delete: `p/PurpleSyncChatDiscovery`, `PurpleSyncChatService`, `PurpleSyncChatCache`, `PurpleSyncChatStartup`,
  `PurpleSyncChatSetup{,Flow,Environment,Transport,Requests,Receipts,Native}` (after moving create builders),
  `PurpleSyncChatCarry`, `Move`, `MoveStore`, `MoveIO` (after extracting `RecordReceipt` and the upload Permit into
  `PurpleSyncReader`/`PurpleSyncAutoUpload`), `MoveRequests`, `MoveCore` (replace the `hash` use in
  `PurpleSyncRetainedFiles:44`), `MoveConsent`, `MoveRecovery`, `MoveCleanup`, `Change`, `Flows`, `FlowPort`,
  `MarkActions`, `ui/...MoveView`, `messenger/PurpleSyncChatMarkUpdates.java`; JNI `purple_jni/sync_chat_move_bridge.cpp`,
  `sync_chat_move_store_bridge.cpp`; trim `sync_chat_bridge.cpp`; `purple_jni/purple.cmake` roster.
- Stub `MoveStore.artifacts()/completed()` callers in `PurpleAccountSyncStore` and `PurpleSyncRecoveryModel` to
  "no carry" before deleting.
- Tests: new `tests/sync_chat_magic/` suite (fake dialog lists, fake transport); rewrite `sync_chat` Facts/IO/Record/
  Upload cases; delete Discovery/Request/Model/Service/Alignment/Setup*/Activation/Screen cases and the move, carry,
  change, consent, marks, cleanup, recovery, races, wire suites with their `run_sync_chat_*.py` drivers; update
  `purple/host_suites.json` expects in the same commits; `tests/diag_log` limiter cases.
- Docs: rewrite `docs/sync-chat.md`, update `sync-auto.md`, `rpc-retry.md`; delete `sync-chat-move*.md`,
  `sync-chat-marks.md`, `sync-chat-cleanup.md`, `sync-chat-recovery.md`.

### Desktop (`P/` = Telegram/SourceFiles/purple/)
- New `P/purple_sync_magic_chat.{h,cpp}` (lookup, list-loaded waiting, rebind, diag), `P/purple_sync_magic_create.{h,cpp}`
  (create/archive/mute steps, reusing `create_requests` builders for archive/mute), `P/purple_sync_chat_binding.{h,cpp}`
  (sync/chat.json), `P/purple_sync_diag.{h,cpp}`.
- `P/purple_sync_request_sender.{h,cpp}`: persisted per-method deadline gate (replaces the RAM-only map as the
  authority; RAM notices stay for status).
- `P/purple_sync_chat_service.{h,cpp}`: keep public names (`EnsureSyncChatResolved`, `CheckSyncChatBeforePost`,
  status), body becomes the lookup; delete `DialogsFor`, `Run` search dispatch, Review* entry points (:1226-1278),
  `ResumeSyncChatCarry` call (:1300).
- `P/purple_sync_chat_requests.{h,cpp}`: delete global/search/hint builders; keep exact read.
- `P/purple_sync_auto_service.cpp` (:249-280, :536), `P/purple_sync_config_upload.cpp` (:228): non-Self bound chat; problem hold.
- `P/purple_sync.cpp`: delete `SearchForSettingsOffer`/`OfferNewerSettingsFromSavedMessages` search
  (:704-744, :1277); `window/window_session_controller.cpp:1626` drops the call (upstream file, one-line removal).
- `P/purple_sync_chat_setup_box.{h,cpp}`, `P/purple_sync_setup_box.cpp` (:1005 forced resolve for every account,
  :1067): rewrite the Sync chat box as on Android; drop move/recovery/follow UI (~400 lines).
- `P/purple_history.cpp` (:113-150): hide bound chat without a running preset.
- `P/purple_sync_chat_store.cpp`, `P/purple_sync_local_store.cpp` (:872-877, :1235): stop opening carry.json/recovery.json.
- `P/purple_sync_chat_carry_gate.h`: keep the account RAM write fence (live in 7 files), drop carry naming.
- Delete: `purple_sync_chat_discovery`, `_cache`, `_carry_{executor,io,owner,service,store}`, `_cleanup`, `_consent`,
  `_mark_actions`, `_mark_transport`, `_recovery`, `_native{,_io,_resolver}`, `_create_store`, `_setup{,_flow,
  _service,_transport,_requests}` (after moving the create builders), `_peer_request` if only discovery uses it.
- `Telegram/cmake/purple.cmake` (:249-321, :349-385): roster.
- Tests: new `purple/test_sync_magic_chat.py` fixture and cases; delete discovery/completeness/alignment/native/
  setup*/move/carry/marks/consent/cleanup/recovery gates; update `purple/host_gates.json` and
  `purple/sync_carry_units.py` in the same commits; `P/tests/sync_chat_move_file_cases.cpp` deleted.
- Docs: rewrite `docs/purple/sync_chat.md`, `sync_chat_service.md`, `sync_request_retries.md`, `sync.md`; delete
  `sync_chat_move.md`, `sync_chat_wiring.md`, `sync_chat_native.md`, `sync_chat_setup.md`; trim `sync_chat_io.md`.

## 7. Commit plan

Core (purple-core repo, then gitlink bumps):
1. K1 `Add the Magic Purple Sync lookup policy and binding codec` (+tests, doc).
2. K2 `Let automatic sync use a bound non-Self chat and pause on chat problems` (+tests).
3. K3 `Remove sync-chat marks, resolution and carry` (+tests, docs). Lands after both clients stop calling them.

Android (P2), each commit host-qualified with `run_host_suites.py`:
1. A1 `Rate-limit repeated diagnostic lines` (independent; can land first).
2. A2 `Bump purple-core for the magic sync chat policy` (K1+K2; JNI bridge additions).
3. A3 `Find or create the Magic Purple Sync chat from the local chat list` (new service, binding, wait gate, diag;
   facade rerouted; search builders and launch offer search removed).
4. A4 `Publish into the magic chat and move automatic sync off Saved Messages` (gate change, migration).
5. A5 `Rework the Sync chat screen around the magic chat` (status, once-problems, override, show toggle).
6. A6 `Hide the sync chat from Purple chat lists`.
7. A7 `Delete sync-chat search, marks, consent and carry code` (+suite and host_suites.json removals; bump core to K3).
8. A8 docs folded into each commit; final doc sweep only if needed.

Desktop (P3): D1..D8 mirror A1..A8. Per the shared-build rule, every commit that touches `purple.cmake` (D2, D3, D7)
voids the Debug manifest, so the chain D2..D7 is qualified as a whole before committing them in order.

## 8. Test plan

Core: eligibility matrix (each refusal and each reason), oldest pick with ties and mixed eligible/ineligible,
PlanSyncChatLookup over binding modes x list states (partial main, partial archive, archive hit with partial main,
absent complete, ineligible only, older duplicate appears), binding codec round trip and unknown fields, wait deadline
monotonic merge, Gate with v2 channel chat, Hold(SyncChatProblem) notice once, migration parent rule.

Clients (host, fake transport and fake dialog lists):
- No search ever: across startup, screen, manual, auto and post triggers, the request log contains no
  `messages.searchGlobal` or `messages.search` (negative control: the old code path fails this assertion).
- Partial lists never create; archive hit with partial main uses the chat; absent with complete lists creates once.
- Uncertain create blocks re-create for 24 h, then the chat appearing via Updates binds it.
- Flood on any sync method persists `retryAt`; after a simulated restart all five triggers refuse dispatch until it
  passes; 30 s floods retry inline once.
- Problem codes (chat gone, left, extra member, extra admin, username added, foreign record, forwarded record,
  via-bot record, creation limit) pause automatic sync, show once across restarts, and resume only on user action;
  transient conditions never pause.
- Records from another sender or forwarded are never applied (inventory unchanged, apply not called).
- Convergence: two bindings to different eligible chats both rebind to the lower id and republish.
- Migration: v1 Self with and without pending stage; empty new chat publishes; same-space records apply/conflict;
  different space raises space_mismatch; Saved Messages is never read afterwards.
- Hidden predicate: bound chat absent from main/archive/folder lists, present when the toggle is on.
- Diag: each event shape, vocabulary refusal of free text, limiter (3 per 10 min, count line, LRU eviction).

Native (after GO, on a test account, never the owner's main account): first run creates one archived muted megagroup;
second device finds it without creating; restart during a flood wait sends nothing; GUI evidence of the screen and
hidden list.

## 9. Open questions

- Q1 Legacy "Send after Save" export still posts to Saved Messages (Android `PurpleSync` legacy route, Desktop export).
  Options: retarget it to the bound chat (recommended: same record format, otherwise it creates records nobody reads),
  or remove the toggle.
- Q2 Manual override scope. Options: Saved Messages plus chats where the user is the only member (recommended: keeps the
  no-consent design, no reader disclosure UI to maintain), or any chat the user can post in, with a one-time reader
  warning (needs the consent UI we are deleting).
- Q3 Title matches exist but none is eligible on an unbound device. Options: Problem, no create (recommended: avoids a
  silent second chat when the user's real one has an extra member), or skip them and create a new one.
- Q4 Different sync space found in the magic chat during migration. Options: show once and offer Join (recommended;
  rare, since all devices shared one Saved Messages space), or auto-join and publish ours as a child.
- Q5 Mixed versions: older Purple builds on other devices keep reading Saved Messages and drift apart until updated.
  Recommended: accept, and say so in the release notes; no bridge code.
- Q6 Absence proof on Android needs the whole main list loaded (upstream pages by 100 on scroll). Recommended: archive
  first, then paced upstream page loads only on an unbound device, once. Alternative: one `messages.getAllChats` call
  (not a search, one request for all groups) as the absence proof; cheaper on big accounts but departs from rule 1.
  - **Resolved in R2 (2026-11-03): the list rule.** The getAllChats alternative was built first and failed on the
    production server: the method is not in layer 232 and the server answers 400 INPUT_METHOD_INVALID
    (`results/native-magic.md`). It is removed on both clients and core. The proof is core's `listsComplete`: the
    main list and archive folder fully loaded by upstream's own loaders, read unfiltered (a Work Mode preset cannot
    hide a magic chat from it). Until then the lookup waits and never creates; a bound chat missing from complete
    lists is `chat_gone`.
  - **Error classes (R2).** Core `ClassifySyncChatError`: only FLOOD_WAIT, FLOOD_PREMIUM_WAIT and SLOWMODE_WAIT are
    waits kept in the binding. Timeouts, network failures, 5xx and 401 are transient, retried with the short backoff
    and never kept. Any other error is refused, a problem shown once that pauses automatic sync
    (`SyncChatRefusalProblem`: `create_limit`, `chat_gone` for a private channel's full info, else
    `request_refused`). A refused read of a candidate other than the bound chat only rejects that candidate.

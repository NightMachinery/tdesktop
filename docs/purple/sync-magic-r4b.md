# Magic Purple Sync R4b: first use of the sync chat

Requirements and state for finishing R4b. Read [sync-magic-design.md](sync-magic-design.md) for the background: the
magic sync chat is a hidden, archived, muted supergroup titled "Magic Purple Sync" that an account's devices share to
sync settings. "D1" is the owner's test account and device pair. "Space" is a sync stream: records and device states
belong to one space. "First use" is a device acting on the chat while it has no state in the chat's space.

## Owner policy

New logins are fully automatic; existing apps get at most one confirmation.

1. Chat empty, local settings non-default: automatic sync publishes the first record itself, with no manual Publish.
2. Chat empty, local settings default: publish nothing, wait for a device with real settings.
3. Chat has records, local settings default: join and apply automatically, no prompt.
4. Chat has records, local settings non-default:
   - same meaning as the chat's head: join silently;
   - different: ONE confirmation, "Use the synced settings" (join, replace local; History/Undo keeps the old file) or
     "Use this device's settings" (publish them as the successor). Never shown again once answered.

Two customised fresh devices racing into an empty chat: strict-successor rules leave one head, and the other device
becomes case 4.

## Findings from the device run on D1 (Desktop R3 build, Android R3/R4 builds)

Find and bind passed. Check on Desktop then refused Android's record with "The selected sync chat has sync records this
device cannot safely use: ambiguous, unreadable, or in another sync space. Nothing was changed.", with no reason code
and no way forward. Desktop keeps its pre-magic `sync/state.json` and old carry files in the OLD space. Android's first
publish after moving to magic was AccountUnbound and started a FRESH space, although both devices shared one before.

Required:

1. Moving to magic keeps the space. A device with a pre-magic state migrates it into the magic chat (MoveSyncChat) and
   continues in the same space, with no question. Find out why Android ended AccountUnbound: did the move drop the old
   `state.json` or fail to read it, or does auto's `CheckSyncAccountBinding(state, bindingToken)` fail after the move?
   Then fix it.
2. A real space mismatch after that is owner case 4: the one-time choice, never a dead-end refusal. The old state is
   archived with the existing Start again machinery (History, settings and the chat binding stay), and the device then
   acts as an unbound device.
3. Every refusal on the Check screen gets a specific diag reason code and a specific line of text.
4. Sync text no longer names Saved Messages as the sync chat. Strings about Saved Messages as a chat choice stay.
5. Leftover pre-magic carry files (`carry.json`, `carry-cache.json`, `carry-objects`, `carry-history`, the startup
   admission "carry-recovery-required") must not block first use, Check, automatic sync or publish in magic mode.
6. Host scenario: two devices that shared a space before magic both migrate and land in one space with zero prompts.
7. D1's exact state must resolve through the fixed builds with no manual file edits: the chat holds Android's
   fresh-space record, Desktop holds the old-space state, and Desktop shows the one-time choice.

## purple-core is done

Branch `sync-magic-r4b` of purple-core, commit 724315c7cf878e28870a7932d210fb8bdb2f87e5. Read its
`docs/sync-config-flow.md` ("First use") and `docs/sync-auto.md`. New functions live in `purple_sync_config_flow.{h,cpp}`,
so no client build list changes for them.

- `IsDefaultSyncSettings(file)`: no file, or it parses with no error or warning to the empty file's Settings. It
  ignores `version` and the file-local `[sync]` table; keys the parser ignores count as defaults.
- `SameSyncSettingsMeaning(a, b)`.
- `SyncFirstUseInputOf(inventory, local)` gives an optional `SyncFirstUseInput {local, groupEmpty, head, choice}`.
- `PlanSyncFirstUse(input)` gives `SyncFirstUseAction {Wait, Publish, Join, JoinSilently, Ask}`. The user's answer goes
  in `input.choice` (`SyncFirstUseChoice::Synced | ThisDevice`).
- `SyncConfigReview.reason` and `SyncConfigReviewReasonCode(reason)`: `space_mismatch`, `unreadable`, `ambiguous`,
  `newer_schema`, `heads_unclear`, `own_records`.
- `PlanSyncAuto` is unchanged: it still holds `AccountUnbound` without a state. The client runs first use there.

If a client needs a core change, do not make it in the client: stop and say what is needed.

## Client behaviour

When an account has no state in the chat's space (AccountUnbound with a bound magic chat, or a review with reason
`space_mismatch`), read the inventory, call `SyncFirstUseInputOf` and `PlanSyncFirstUse`, and act through the EXISTING
publish, apply and join machinery:

- Wait: no request; a short status line saying it waits for a device with your settings.
- Publish: the first publish, no confirm dialog, no manual Publish press.
- Join: apply the chat's head with History, as the existing "use synced settings" path does.
- JoinSilently: join the space without touching the local file. If the bytes differ although the meaning is the same,
  Desktop publishes a successor (`sameBytes`, found by its host cases).
- Ask: ONE choice, in the sync screen plus an attention line in the automatic status. No dialog pops up on its own.
  Feed the answer back as `SyncFirstUseChoice` and act.

Once answered, or once a state exists in the chat's space, it can never be asked again: the state settles it.

## Android state

Base: the head of `sync-magic-r4b`. It has R4a and R4a2, which matter here:

- Durable sync paths walk from the app's files directory, never from "/" (Android refuses reads of "/"). Link checks
  look only at the last path component (`PurpleAccountFileSnapshot.notLink`), because the files directory is reached
  through the link `/data/user/0`. Never compare whole canonical paths.
- D1's Android has a post that reached the chat with a failed local commit. Its store opens as it is, Check shows the
  post as unfinished, and Finish sending confirms the found record without sending it again. First use must not
  disturb a pending post.
- `PurpleSyncMagicScenarioCases` is the end-to-end host scenario: real store, retention and known-files code with the
  files directory behind a link, fake transport only. `PurpleSyncMagicPublishCases` has the fake chat. Build on these.

To do (none of it started):

1. Bump the gitlink `TMessagesProj/jni/purple` to the core commit above, and bridge the new core functions (the JNI
   bridge, `PurpleSyncCore`).
2. The client behaviour above, in the automatic sync service and the Check path of the Sync screen, with the choice UI
   and strings (`purple_strings.xml`).
3. The migration-keeps-space investigation, item 1 of the findings. Start from the Android move path and the auto
   binding check. State the root cause in your report.
4. Named refusals (diag reason codes and text), items 2 and 3. Carry leftovers, item 5, if Android has the same block.
5. Host scenarios on the real store: each first-use action from a no-state device; two devices that shared a space
   before magic both migrate and land in one space with zero prompts; the race of two customised fresh devices into an
   empty chat (one head, the other ends in Ask); the D1 state, including a pending post.
6. A negative control for each rule: revert it, see the case fail, restore it. List them.
7. `docs/sync-auto.md` (the "use manual Check to join its sync stream" text goes), `docs/sync-chat.md`.

Trap: `showDialog` dismisses the visible dialog, so a picker over a dialog must call `show()` directly.

## Desktop state

Base: the head of `sync-magic-r4b`. Its commit "Name each sync refusal and stop naming Saved Messages as the sync chat"
has the gitlink, named refusals and wording. The commit labelled WIP above it has first use; the commits after it finish
it without rewriting it:

- `ReviewSyncFirstUse`, `SyncCarryLeftoversPresent` (config_review); `PlanSyncFirstUseArchive`,
  `StartAgainSyncAccount(firstUse)`, `RetireSyncCarryLeftovers` to `sync/carry-retired-<date>/` (account_recovery);
  `ApplySyncFirstUseJoin` (config_apply); `SetDesktopSyncAutoBeforeState` (auto_store); `AutoSession` FirstUse,
  FirstUseRun, FirstUseJoined, FirstUseFinish, NoStateEvent, RetireCarry, and PauseOnForeignSpace leading to first use;
  `RunSyncFirstUse`, `SyncFirstUseResultOf`; box RunFirstUse, FirstUse and the useSynced and useThisDevice buttons;
  strings `lng_purple_sync_first_*`, `lng_purple_auto_first_*`; diag words. The behaviour is described in
  [sync_chat.md](sync_chat.md#first-use).
- Fixed after the WIP review: automatic sync started first use only when the chat held no record of the state's own
  space, so the loser of a race (its own record beside the winner's) got a dead-end `space_mismatch` refusal instead of
  the one-time choice; it now uses `ReviewSyncFirstUse`, as Check does. First use refuses while a Start again journal
  is present. A broken local file (Invalid, or last-good in use) no longer reads as "waiting for a device with your
  settings" with "default settings"; it is named as a file error.
- Host cases: FirstUseActions, FirstUseD1 (both answers, carry admission, and a core review that is Ready afterwards),
  FirstUseMigration (both states moved from an old chat by `MoveSyncStateToChat`, one space, zero prompts) and
  FirstUseRace in `purple/test_sync_config_publish.cpp` (gate setup-publish); FirstUseJoinAppliesHead and
  AutoBeforeState in `purple/test_sync_auto.cpp`. The routes gate `purple/test_sync_chat_routes.py` pins the first-use
  routes, with a negative mutation for each.
- After review CHANGES (P1-1): the gitlink is at core 493d0dbb03, whose `SyncFirstUseInput::headKey` comes from the
  current (maximal) heads. `ReviewSyncFirstUse` uses it as the head key and `{ headKey }` as the expected parents and
  no longer reads `ExtractSyncConfigHeads`. Cases: FirstUseCurrentHead (setup-publish) and
  FirstUseJoinAppliesCurrentHead (test_sync_auto), with R1 and its successor R2 from another install.
- `purple.cmake` is unchanged.

Still to run locally (the session that finished the code had no Qt and no Debug build, so no compiled gate ran there):

1. The Debug build and the full host gate set (`purple/run_host_gates.py`), including setup-publish, test_sync_auto
   (main and worker) and cases-sync-magic-chat with the changed cases.
2. Negative controls for the compiled cases, each a revert to see fail and restore. None has a result yet:
   - N1 `input->choice = choice;` to `SyncFirstUseChoice::None`, config_review, setup-publish;
   - N2 `} else if (*selected != parsed.state.space) {` to `} else if (false) {`, config_review, setup-publish;
   - N3 the `SyncCarryLeftoversPresent` early return to `if (true)`, account_recovery, setup-publish;
   - N4 `recovery.status != SyncAccountRecoveryStatus::Bound ||` to `true ||`, account_recovery, setup-publish;
   - N5 `SelectedSyncSpace(inventory)` to `OneSyncSpace(inventory)`, publish_flow, setup-publish;
   - N6 `result.request.expectedParents` to an empty vector, config_review, setup-publish;
   - N7 `result.sameBytes` to `bool(input->head)`, config_review, setup-publish;
   - N8 the owner-only `QDir().mkdir` to a plain one, account_recovery, setup-publish;
   - N9 `head.key == *headKey` to `!=`, config_apply, test_sync_auto;
   - N10 `record.enabled = enabled;` to `false`, auto_store, test_sync_auto.
3. The RelWithDebInfo stage, signing, and the GUI and device checks of D1.

### Handoff after review CHANGES (branch head after ab58e54)

Done on this branch, none of it compiled yet: P2-1 (7ab8642, `sync/first-use.json` keeps the answer and a started
join), P2-3 (7c8b300, 15-minute limit without a state), P2-2 (52277c4, named first-use failures), P2-5 Desktop part
(81f0bd2, MigrationKeepsSharedSpace), P3-2, P3-4, P3-5, P3-6 (220b76c, df8010a, d734565, 341efca), P1-1 client side
(45fcb0e gitlink at core 493d0dbb03 and `headKey`; ab58e54 FirstUseCurrentHead and FirstUseJoinAppliesCurrentHead).
Static gates all pass (`test_sync_chat_routes.py`: 79 routes, 102 negative mutations).

Remaining:

1. Debug build and `purple/run_host_gates.py` in full; fix whatever the new compiled cases expose. Riskiest:
   `Drive` and MigrationKeepsSharedSpace (test_sync_auto), the Ready and verdict checks in FirstUseMigration and
   FirstUseD1, FirstUseMarkKeepsAnswer, FirstUseCurrentHead (setup-publish), sync-diag vocabulary (new words).
2. Negative controls N1 to N10 above, run and recorded.
3. Bump the gitlink to the core commit that fixes `SameSyncSettingsMeaning` line numbers (P2-4); no client change
   expected.
4. Core gap to raise: when current heads disagree, core gives no `headKey`, so "Use this device's settings" publishes
   with empty expected parents and core's publish gate (which compares them with its own choice's parents) likely
   refuses it every time (`chat_changed`). Core should name the parents for that case.
5. P3-3: use the actual cause's reason code (newer schema in `PauseOnForeignSpace` fallback, `!recovery` and the ledger
   mismatch in `ReviewSyncConfig`).
6. P3-1: reword the WIP subject of e647b5f at the next rebase (owner's call).
7. RelWithDebInfo stage, signing, GUI and D1 device checks.

Keep `purple.cmake` unchanged if you can, and say why if you cannot.

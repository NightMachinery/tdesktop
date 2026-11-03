# Review: Desktop and purple-core Magic Purple Sync R4b (first use)

Scope: Desktop `7c361c5728..f53526974f` on `sync-magic-r4b` (R3 was approved at
7c361c5728) and purple-core `sync-magic-r4b` `f7c379fdd4..724315c7cf`. Read
against AGENTS.md, [sync-magic-r4b.md](sync-magic-r4b.md),
[sync-magic-design.md](sync-magic-design.md) and [sync_chat.md](sync_chat.md).
This was a source review only. The VM cannot build, so no gate ran here, and
every P1 below comes with a code trace instead.

Verdict: CHANGES. P1=1 P2=5 P3=6.

What holds up well: the policy table in `PlanSyncFirstUse` matches the owner's
four cases. The race of two fresh devices settles deterministically on core's
selected space, and the loser reaches Ask, not a dead end (fixed in 27d75d5).
Migration keeps the space, because `MoveSyncStateToChat` keeps `space` and
`SyncConfigAwaitsMovedPublish` republishes into the empty magic chat. Carry
leftovers are moved into an owner-only folder under the account lock, and
nothing is deleted. A missing state while a pending stage still waits gets no
action. First use sends no search request. A partial inventory never plans,
since core needs `InventoryStatus == Ready`. No upstream file is touched, so
no hook can be wrong. The diff has no ids, names or paths that leak anything.
The archive fix (f535269) is correct, and its routes-gate case has a negative
mutation.

## P1

### P1-1. First use counts one head per install, so most multi-device spaces cannot be joined automatically

Core `SyncFirstUseInputOf` sets `head` only when `ExtractSyncConfigHeads`
returns exactly one entry. Desktop `ReviewSyncFirstUse` sets `headKey` under the
same condition. But `ExtractSyncConfigHeads` returns the head of every config
group in the space, one per install. It does not drop heads that another
install's head already supersedes. `PlanConfigSync` does drop them (`Classify`
marks a head `Stale` when another head's lineage holds it). So once two devices
have each published once, which happens as soon as the owner edits a setting
on a second device, first use sees "more than one head" forever.

Trace (an ordinary account, and nothing unusual about it):

1. Desktop D is customised and publishes R1 (install iD, space W). Case 1.
2. Android A logs in with defaults and joins, applying R1. Case 3. Later the
   owner changes a setting on A, and automatic sync publishes R2 (install iA,
   parents [R1]).
3. The chat now has groups (W, iD) with head R1 and (W, iA) with head R2. R1 is
   in R2's lineage.
4. A new login E has default settings. `ReviewSyncFirstUse`: need NoState.
   `SyncFirstUseInputOf` gets 2 heads, so `head = nullopt`. groupEmpty is false
   and the local file is default, so the action is Join. `headKey = nullopt`.
5. `AutoSession::FirstUseRun` runs `SyncConfigPublish` with `initializeOnly`.
   That writes a state in W (new install, empty base) and returns AlreadySynced.
   Then comes `FirstUseJoined`, then `ApplySyncFirstUseJoin`. Its
   `ReviewSyncConfig` plan has `maximal = {R2}`, and R2 is `Unrelated` (empty
   base, so it is not `clean`, and `known` is empty), so the verdict is
   `Choose` with `offered = [R2]`. It is not `Adopt`, and `headKey` is null, so
   `find_if` finds nothing, `record` is null, and the result is
   `InvalidChoice`. `FirstUseFinish(Failed)`.
6. The next trigger opens the new state. `PlanSyncAuto` sees `Choose` and does
   `Hold(Conflict)`. The status says "needs attention", and the owner has to
   resolve a conflict by hand on a fresh login with default settings.

Policy violations: case 3 ("new logins fully automatic") fails. In case 4, a
device whose settings mean the same as R2 gets `Ask` instead of
`JoinSilently`, because `head` is null. Choosing "Use the synced settings"
in that Ask fails the same way as step 5 (after the old state has already been
archived, when it was OtherSpace), and the choice comes back as a conflict
review. Only "Use this device's settings" works.

No test covers it. Every first-use case (core `TestFirstUse`, Desktop
FirstUseActions, FirstUseD1, FirstUseRace and FirstUseJoinAppliesHead) has
exactly one remote install in the selected space. `FirstUseJoinAppliesHead`
even pins "no head key, so not applied" as expected behaviour.

Needed (core first, per the R4b rule that a client does not patch core):
`SyncFirstUseInputOf` should take the maximal heads, dropping every head held
in another head's lineage, the same way `Classify` does. It should set `head`
when one maximal head remains, or when all maximal heads share one
fingerprint. Core should also return that head's key, and Desktop should use
it for `headKey` and `expectedParents` instead of re-deriving them from
`ExtractSyncConfigHeads`. Add a core case and a Desktop case with R1 (iD) and
R2 (iA, parent R1) covering Join, JoinSilently and "Use the synced settings".

## P2

### P2-1. An answered choice can be asked again after a failed or interrupted step

The answer lives only in RAM (`input.choice` passed to `FirstUseRun`). For
OtherSpace, the steps are: archive the state (sync), publish or initialize
(async, through the publisher), then apply (Join). If the run dies or fails
between archive and the publisher's `Initialize`, the device has no state. The
next pass is NoState with `SyncFirstUseChoice::None` and shows Ask again.
Causes include a crash, a quit, `NeedsReview` from the flow's queue or recovery
guards, or `AccountUnavailable`. If it dies between `Initialize` and the apply
(Join), the new state has an empty base. The normal path then gives `Choose`
and `Hold(Conflict)`: the same question comes back as a conflict review. For a
default fresh login (case 3), that same window gives a new login a conflict
prompt. Settings are not lost (History holds the file, and no path publishes
the local file over the group), but "never asked again once answered" does
not hold. Persisting the choice next to the missing state (as
`SetDesktopSyncAutoBeforeState` does for the switch), or writing the base
during initialize for Join, would close both windows.

### P2-2. First-use failures carry no reason code and say only "did not finish"

`FirstUseFinish(Failed)` is reached from nine places: no lease, incomplete
scan, journal present, archive refused, initialize or publish refused, apply
refused, `_inFlight`, unbound, and the default fallback. Each one shows
`lng_purple_sync_first_failed` ("Setting up sync did not finish. Check the sync
chat again.") in the box. The diag lines carry only `step=archive|publish|join
outcome=error`, with no reason. Only the apply step names its reason
(`SyncConfigApplyFailureText`). This breaks the R4b rule that every refusal on
the screen has a specific reason. A refusal that does not go away (for example
`PlanSyncFirstUseArchive` returning nullopt for an incomplete pending stage, or
a non-empty send queue) turns into a dead end that says "check again" forever.
That is the same class as the old `space_mismatch` dead end. The automatic
status is not silent (no state, or a stale `record.active`, gives
`lng_purple_auto_review`), so this is not the Android "silent while On" bug.
But the user cannot tell why it fails. Give `SyncFirstUseResult` a reason code,
log it with `SyncDiagReason`, and add one line of text per reason.

### P2-3. A device with no state scans the chat on every activation, bypassing the 15-minute lifecycle limit

`AutoSession::Event` does `if (!Open()) { Stop(); return NoStateEvent(event); }`,
and `NoStateEvent` calls `FirstUse(std::nullopt, ...)` for every event except
Disable. That starts a fresh `SyncAccountInventory` scan each time. The
lifecycle limit lives in `DesktopSyncAutoStore::Transition`, which never runs
without a store. So a device that Waits (empty chat, defaults) or Asks (NoState
with records) runs a full history scan, plus candidate reads, on every
`appDeactivatedValue(false)` (each window focus), every Unlock, and twice per
save (`Saved()` fires LocalSaved right away and again after `kSaveDelay`).
Waits stay honoured through the request gate, so this is a volume problem and
nothing gets lost. But it brings back the request pattern that section 1 of the
design was written to stop. Keep the last first-use outcome, with its time,
beside the missing state and apply the same lifecycle limit to it. Only
Enable, a manual Check and SyncChatStatusChanges should bypass that.

### P2-4. `SameSyncSettingsMeaning` compares source line numbers, so comments shift "meaning"

Core's `SettingsMeaning` compares whole `Settings` values with the new
defaulted `operator==`. That covers `ScheduleRule::sourceLine`,
`ScheduleRuleset::sourceLine` and `sourceIndex`, and
`ScreenTimeBudget::sourceIndex`. `ParseSettings` fills these from the file
position (`fields->source().begin.line`). Two files with the same schedule
rules, one with an extra comment or blank line above `[[schedule.rules]]`, are
therefore "different". Case 4 then shows Ask instead of JoinSilently, which
breaks the core doc's promise that "comments, spacing and spelled-out defaults
never prompt". Schedules are a core Purple feature, so the owner's files are
likely to have rules. `IsDefaultSyncSettings` is unaffected, since a default
file has no rules. Clear the position fields in `SettingsMeaning` (or compare
them with a meaning-only equality), and add a core case: the same rules with
an extra comment line before them.

### P2-5. The host cases do not prove the migration and join claims

- `FirstUseMigration` makes `second` join with `initializeOnly`, so its state
  has an empty base. After the move, its local file (`kCustomOther`) differs
  from `first`'s head (`kCustom`), so its real automatic plan is `Choose`, then
  `Hold(Conflict)`: a prompt. The case still prints `prompts=0`, because it
  counts only first-use Ask (`need` is None, so Ask is impossible) and checks
  `checked.status == Ready`, never the verdict. The owner's scenario ("two
  devices that shared a space before magic, zero prompts") needs states with
  real bases. Both devices should hold the same synced head, both move, one
  republishes through `SyncConfigAwaitsMovedPublish`, and then the case should
  assert the other device's verdict is `Adopt` or `UpToDate`.
- `FirstUseJoinAppliesHead` only covers one remote head, and treats a missing
  `headKey` as an accepted non-apply (see P1-1).
- None of the ten negative controls (N1 to N10) has been run, and the compiled
  gates (setup-publish, test_sync_auto main and worker,
  cases-sync-magic-chat) have not run either. The doc says so honestly. It is
  still a merge blocker until they run.

## P3

- P3-1. Commit e647b5f keeps the subject "WIP: ..." and a body saying "Negative
  controls, docs and the full gate set are still to do", although docs were
  added later. Read together with 27d75d5, b8ff040 and 84e003f, the code is one
  coherent change. The routes, the diag words, the strings and the doc all
  match what the WIP introduced, and the later commits fix rather than
  contradict it. Still, the subject breaks the AGENTS.md subject rule. Reword it
  when the branch is next rebased.
- P3-2. `sync_chat.md` and the `SelectedSyncSpace` WHY comment say every device
  joins "the earliest space". Core picks the lowest space id
  (`CompareSyncSpaceIds`). That is earliest only for
  `FormatTimeOrderedSyncSpaceId` ids made on correct clocks. It is
  deterministic, which is what matters, but say "lowest id" so nobody reasons
  from "earliest" (a device with a fast clock loses the race; a stray record
  with a lower id moves the whole group).
- P3-3. When core cannot plan an OtherSpace inventory (for example a newer
  schema head), `PauseOnForeignSpace` falls back to the normal check, which
  logs and shows `space_mismatch`. The real blocker is `newer_schema`.
  Similarly, a failed recovery inspection (`!recovery`) in `ReviewSyncConfig`
  is labelled `ambiguous`, and a ledger mismatch in the same space is labelled
  `own_records`. Use the code of the actual cause.
- P3-4. A transient failure of a first-use scan on a device with no state
  (network, timeout) ends `Failed`. The automatic status then reads
  `lng_purple_auto_review` ("needs attention ... conflicting versions"). It
  does not pause, but it words a transient error as a problem. Give it the
  retry or wait wording.
- P3-5. The box answers Ask with `state->completed`, the inventory from the
  Check that showed the choice, which may be minutes old. "Use this device's
  settings" then names the heads of that scan as parents. If another device
  published in between, the result is a concurrent head and a later conflict.
  Rescan when the answer is given (pass `std::nullopt` to `RunSyncFirstUse`).
- P3-6. `StartAutomaticSettingsSync` has the same launch pattern that f535269
  fixes for the archive service (subscribing only to `accountsChanges()`). It
  works today only because the startup lookup's `SyncChatStatusChanges` fires
  SessionReady. Add the same `activeSessionChanges()` refresh, or note why it
  is not needed.

## Checked and fine

- Default definition: `IsDefaultSyncSettings` treats as default: an absent
  file; an empty file; comments; `version`; the file-local `[sync]` table;
  spelled-out defaults. Unknown keys also count as default, which is
  documented, and the only false positive is a key from a newer build that
  this build ignores (History keeps the file on Join). A file that is Invalid,
  on last-good, or not loadable by Desktop (`SettingsTextLoads`) waits and is
  named as a file error.
- Settings safety on Join: `ApplySyncFirstUseJoin` goes through
  `ApplySyncConfigChoice` with a replacement review, so History and Undo are
  kept. JoinSilently with the same bytes never writes. JoinSilently with
  different bytes publishes this device's text as a successor (the owner's
  rule). No path publishes the local file over the group without the answer
  "Use this device's settings".
- Archive for OtherSpace: `PlanSyncFirstUseArchive` keeps History, the settings
  file and the binding, and refuses an incomplete pending stage. Start again
  journals block first use in both the automatic and the box paths.
- Foreign or forwarded records: first use uses the same inventory and
  ownership path, and a magic problem unbinds (`SyncMagicBound` needs
  `problem == None`), so a paused chat runs no first use.
- Enable before a state exists: `SetDesktopSyncAutoBeforeState` writes the
  switch under the account lock only while `state.json` is missing, and the
  store opens enabled afterwards (AutoBeforeState).
- Privacy: no ids, names, phones or local paths in code, tests or docs. Diag
  fields are codes only.

REVIEW R4b desktop: CHANGES P1=1 P2=5 P3=6

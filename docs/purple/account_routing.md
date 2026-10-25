# Desktop account routing and startup ownership

The shared account model now has a Desktop file adapter. Startup gathers stable
production/test account identities, directory-presence evidence and root-store
binding matches from the existing client APIs. The startup owner never follows
the active account, a later sign-in or a live global-account edit.

`DesktopAccountConfig` reads both owner copies with presence and read-error
classification. Broken links, non-regular owner inputs and an aliased accounts
parent refuse startup ownership. Its ordered plan writes copy 2 before copy 1
using QSaveFile, exact paired-input preconditions before each write and another
check before commit. Each write is read back. A partial write remains held and
is recovered from the next startup snapshot, rather than being silently retried
in the same process. Existing root settings, sync state and History never move.
The check-to-rename interval remains a race against a noncooperating external
writer; this is not a cross-file transaction or an observed disk-isolation claim.

Settings > Advanced > Purple has an original-file ownership row and an eligible
signed-in account choice. Accounts that already have a directory are excluded.
The restored-owner notice is acknowledged when that box is opened. A confirmed
owner survives sign-out. The initial no-account state waits for the first sign-in.

Paths come from the accepted shared resolver. The loader does not create a
settings file on read. It keeps per-file diagnostics and last-good copies, loads
the selected global file even when its account is signed out, and layers each
other account through the shared per-key implementation. File-local version and
sync settings do not inherit. Global settings and device state are not replaced
by a non-global partial file. Own/follow membership selection and stock membership
and preset-level adapters use the originating session. Notification preview and
Last Seen reads use that account's effective settings. The existing ordered
session refresh is fired for account-file and local-choice changes even when the
device-wide preset resolution is identical.

The watcher follows existing directories, their parents and selected files so
atomic replacement, missing files and directory recreation can trigger refresh.
Cached Settings objects retain stable addresses. Operations capture an account,
path, exact input bytes and routing generation; asynchronous session and account
checks refuse stale destinations. Local choice changes invalidate routing, while
unrelated device-state writes do not invent an account transfer.

## Account boundaries

Legacy schema-2 Send, Share and imports now use the destination-aware review
below. Modern sync setup/apply/History accept supported schemas through captured
impact reviews. The unreviewed replacement path still refuses removed app-wide
or unknown keys. Parser defaults and version constants remain unchanged;
recovery does not migrate settings by copying another account's file.

Sync setup, store, publish, apply, History/Undo and fingerprint persistence now use
explicit validated originating-account files. Root-owner paths remain legacy;
nonroot accounts never fall back to them. Manual single-file export remains a
separate owner-unknown role. The routed local writers and list ticks are described
below. The list-mode controls are described below; the full account editor remains separate work.
Only a unique verified keyed non-root store permits automatic binding restoration.
Root Relink requires complete cloud proof and an explicit action.

Synthetic Qt/file cases qualify the actual adapter and guarded writer in owned
temporary directories. Production-TU compilation is separate from application
linking, native UI observation, disk crash behavior and multi-process isolation.
The complete M2 client feature is not finished until the remaining held routes are
implemented, reviewed and qualified on a coherent current app.

## Reviewed account-row and manual-export boundaries

Manual root export is an explicit owner-unknown single-file role. A confirmed
owner with an unavailable account path refuses export instead of substituting
another account's bytes. Confirmation rechecks role, originating session, routing
generation and the exact file snapshot; completion rechecks before bookkeeping.
Valid account-file manual Send and Share support schema 1 and 2. Reviewed legacy
imports accept both document names. Modern schema-2 sync uses the same account
context.

Last Seen reasons and Last Seen Peek read the Settings window's account's effective
values and write only that account's own file. Send after every save is file-local:
turning it on in the global file does not opt another account in. These three rows
use purpose-specific destinations; legacy global writes and unrelated editors do
not gain permission from them. Missing, invalid or stale routing disables the row.
Queued actions retain the originating session, window, account and file snapshot.

With two or more loaded accounts, row captions distinguish the global default,
inherited values and explicit own values. A pinned non-global Last Seen row offers
Use the global account's value, removing exactly that own key through the shared
Reset operation. Reset is absent on global and file-local rows. Names update with
the signed-in accounts; a signed-out global account is identified honestly.
App-wide Premium, Work Mode and Screen time remain app-wide and describe the
global file on other account screens. Device confirmation preferences stay local.

Stock folder lookup uses the ChatFilters originating session when selecting the
Purple branch. The native nonpremium All-first mapping is unchanged. Synthetic
policy cases exercise the real shared stock decision; actual shortcut/index behavior
and GUI rows remain native-unmeasured until a separately admitted application run.

## Account-specific sync, History and send state

Modern sync setup/check/publish/apply uses the selected originating session's own
settings and validated AccountFiles. Root-owned stores retain sync/state.json and
sync/history; other accounts use accounts/<key>/sync and sync/history. Binding
proof is still required and a lost binding is never silently adopted. There is no
Relink control in the current UI, so unbound stores remain refused rather than
restored without library-stream ownership proof.

Operations capture their account, file bytes, routing generation and real directory
identity. Async post/readback and deferred UI actions recheck that context, binding
and relevant store-head snapshot. History lists/previews/restore and Undo retain
the originating account. Finish-sending counters retain weak session lifetime and a separate captured counter,
so a recreated session slot cannot inherit an older in-flight count.
History retains its existing metadata, 30-entry pruning and verification algorithm;
its explicit access object supplies paths and a live precondition. The file module
uses Qt diagnostics and Qt's system random generator, so real file cases run without
an application or fake session/logger implementation.

Root send fingerprints remain the existing device State fields. Nonroot files use
only last_sent_fingerprint and last_imported_fingerprint in send-state.toml. Invalid,
unreadable, aliased or unknown-format send state holds automatic send. Imported
bytes are also suppressed in memory before marker persistence, preventing an echo
when that write fails. Each observer compares only its own file; initial observation
and an observed missing-to-present transition never queue a send. A later external
replacement/save is debounced for that account, and a signed-out session cannot
supply another account's transport. The existing QFileSystemWatcher may coalesce
external events; an unobserved delete/recreate gap cannot be distinguished from a
replacement save.

Absent settings are read without creation and cannot initialize or publish through
sync setup. The explicit Open action can create a missing non-root account file.
Modern schema-2 sync uses captured account-aware replacement reviews and exact
primary snapshots; automatic application adds the stricter safe-successor gate. The final precondition-to-rename window against a noncooperating writer remains
finite, not a universal filesystem compare-and-swap. GUI, live network/account flows
and a coherent current application need separate qualification.

Automatic send retains the qualifying operation's exact file snapshot and weak
originating-session ownership. Each observation cancels stale/invalid queued work;
only another qualifying save replaces it. Timer dispatch validates that capture
instead of reading newly created bytes. Session-specific receipt tokens prevent
an older completion from clearing a newer session's in-flight operation.

A retained History entry can outlive its originating setup box. The actual restore
callback is wrapped by a setup QObject lifetime guard before any setup-owned state
is read; it then reacquires the selected session and validates the captured target.
Synthetic cases invoke the directly used production queue and QObject guard. They
do not claim a real widget stack or live network reproduction.

## Explicit account file Open

The settings file block belongs to the Settings window's session. Its own-file
row identifies that account and its exact path. A second Open row points at the
selected app-wide account, including when that account is offline. When the window
already belongs to that account there is only one Open row. The immutable original
file owner remains a separate role and is never a fallback destination.

Reading settings, signing in and configuring sync do not create an account file.
Explicit Open creates only a missing non-root account file, with the approved
comment header and `version = 2`. It writes no starter overrides, account ids or
names. A missing root file is not replaced with an account template. Existing
files are opened without changing their version or bytes. The purpose-specific Last Seen and automatic-send rows use the own-file route
described below. Other per-account in-app editing remains held.

Creation captures the owner/local choice inputs, destination account, root and
account directory identities, file input and originating session/window. It writes
a complete temporary file before publishing without replacing an existing file.
On POSIX, directory descriptors and `linkat` prevent symlink traversal at the
created child and destination replacement. Windows uses directory handles without
delete sharing and a move without replacement. Existing links, non-files and read
errors refuse Open. Changed identities or inputs refuse the editor launch, including
a change during notification/reload. A concurrent creator's bytes are preserved.
These finite checks are not a universal cross-process compare-and-swap, and the
external editor has its own later filesystem access after launch.

Creation seeds the file observation without sending or publishing it. Existing
qualified save observations and queued operations keep their established rules.
The account template is schema2. Its later edits can use legacy Send or file-local
send-after-save; initial creation itself never sends. The own-key Last Seen Reset described
above is separate from the routed list-member writes below and from a general
account editor or the reviewed legacy transfer path below.

The account-file batch passes all 58 affected production compiles and 967 genuine
Qt/file checks, including 178 added checks. Native editor dispatch, Windows
execution and the matching full application remain separate validation steps.


## Own-row writes and remaining limits

A normal own-file write keeps its preimage in that file's `.bak` and updates its
`.good` only after the guarded settings publication. Reset leaves unknown,
app-wide, nested same-named keys and the file version in place. The writer binds
owner/local choice, global source, directory identities and exact own bytes. It
checks the originating lifetime after notifications and again around publication.
When a global recovery copy supplies an inherited value, the captured `.good`
bytes are part of that check. A missing own file with an existing recovery copy
stays held for repair; a toggle does not silently replace recovered settings.
Creating a missing file uses the existing exclusive directory-relative creation
route, publishes the header and requested override together, and seeds its save
observation without sending. Later supported saves use the established per-file
fingerprint and originating-session queue policy. Schema2 legacy transport uses the reviewed transfer boundary below.

These source changes still require affected production compilation, genuine
file-case execution and independent review. Existing 58-production/967-file-case
qualification describes the preceding account-file Open batch. Native UI and
Windows execution, a coherent matching application and release are separate.
The finite final check-to-rename interval and nontransactional settings/backup/good
updates are not a universal multi-process filesystem transaction.

The account rows and Reset batch passes all 58 affected production compiles and
1,340 genuine Qt/file checks, including 373 added checks. The matching full
application, native settings interactions and Windows behavior still require
separate validation.


## Choosing the app-wide settings account

The Advanced Purple section shows **App-wide settings from … · Change** when
there are at least two signed-in accounts, or when the current source is signed
out. The picker uses signed-in account names and marks test-server identities.
The current signed-out source remains a labeled choice. Choosing a different
source never changes the original root-file owner or relocates that owner's file.

Preview reads the selected account's actual file, its parser result and, when
needed, that same account's valid recovery copy. It compares list and preset
counts, schedule activity and Local Premium. It says when the selected file has
no app-wide settings, and when a recovery copy supplies the preview. A missing
file with no recovery copy previews defaults; it is not created. An invalid file
without a valid recovery copy is refused instead of borrowing another account.

Confirmation captures the originating window and session, signed-in roster,
current global role, owner record, local choices, selected and previous files,
relevant recovery inputs, directory identities, file identities and routing
generation. Changed or dead contexts require another preview. Observed role ABA
and same-byte atomic replacements invalidate the snapshot. Filesystem identity
checks include POSIX modification/change timestamps; Windows records volume/file
index and last-write identity. They do not
provide an unbounded multi-process compare-and-swap guarantee.

The confirmed writer changes only the root local global-account value and its
managed path comment. It preserves the immutable owner, own/follow overrides,
unknown local fields and unrelated text. Existing inline comments are retained;
live account names and test-server labels come from the UI, not new disk fields.
It does not create settings files, move stores, edit History or fingerprints, or
send settings. Existing ordinary reader recovery-cache maintenance is unchanged.

If the running preset cannot resolve in the selected settings, confirmation
warns that Work Mode will enter Normal. The explicit UI route defers state
notifications, establishes the actual Normal gate while old settings are still
active, and then installs the captured new settings before releasing deferred
notifications. It rechecks lifetime and committed context around activation and
notifications. External edits to local choices keep their existing cached-preset
reload behavior. Local-choice publication and best-effort state persistence are
separate files, so this is not a crash-atomic cross-file transaction. A saved
choice whose context changes is reported as saved but not safely activated; it
is not rolled back over another writer.

The chooser baseline passed 69 affected production compiles and 1,580 genuine
configured-Qt/file checks. Complete source review found one activation-ordering
defect: a detected failed commit must leave the old State/Gate untouched. The
repair uses one production-called ordering seam, with actual-model post-publication
conflict and synchronous lifetime cases in the genuine fixture. Its affected qualification passed both production units and 1,660 genuine
configured-Qt/file checks, including 80 new causal checks. Focused source review
closed the activation-ordering finding. Full Config/Gate/window native behavior
remains unmeasured; helper callback checks do not establish native coverage. The full account editor and unsupported version-two transport
remain separate work.


A saved but rejected commit never invokes the Normal transition or new-settings
activation. The production coordinator requires a successful commit and a current
origin/committed snapshot first, then rechecks after synchronous Normal and model
activation. The facade retains its final notification/context guard. Confirmation
reports a saved choice whose activation did not finish safely, rather than saying
nothing changed. There is no rollback over a newer writer.

### Removing inactive app-wide settings

A non-global account with app-wide keys in its existing file has an explicit removal action in Settings Files. The summary lists registry-derived inactive keys and the account currently supplying app-wide settings. The confirmation names the destination account, explains the effect on devices where that account supplies app-wide settings, and defaults focus to Cancel.

The purpose-specific operation calls the shared lossless removal splice and produces version 2. It retains owned and file-local values, unknown text and unrelated files. It first saves the exact original bytes to that account's `settings.toml.bak`, then publishes the successor. Preview and publication bind the live originating session/window, owner/global role, observed revision, directory identities, exact bytes and native file identities, including an existing backup. A stale or unavailable destination refuses the write. A backup-only or post-publication failure is reported separately and never rolls back over a foreign writer. The final check and rename have a finite race window; backup and main publication are not one cross-file crash transaction.

This action does not enable version 2 sync, imports, general editing or list
forks. Its four changed production units and 2,201 genuine configured-Qt/file
checks pass, including 541 added checks. Complete independent source review found
no material defect. The matching full application, native button behavior and
actual-profile operation still require separate validation.

## Routed app-wide writers and list ticks

Local Premium, preset pins, new lists, schedule, screen-time and Peek edits use a
captured app-wide account file, including when that account is signed out. The
original root-file owner is never a fallback write destination. Dialog writers
retain their capture across callbacks and refuse stale files or routing. A
successful dialog edit advances only that dialog's capture to its committed
snapshot. Peek Apply takes the opening Work Mode editor and its weak session;
changing the app-wide source refuses that dialog before a countdown can start.
The countdown retains the captured destination, editor lifetime and originating
session through completion. A successful Apply advances that same editor's
snapshot for later edits.

Chat-list ticks capture the chat's session, the local list mode and both source
and destination inputs when the menu opens. Follow writes the app-wide file;
Own and Stock write the chat account's file. A first inherited-list edit copies
only the effective member order and member comments through SetListMembers.
Only the changed chat is resolved in its own session. Titles, kinds, other lists,
presets and other inherited settings are not copied. Removing the final member
writes an explicit empty array. Ticks never use Reset, including in a whole file
that defines the list. The chooser and explicit deletion rules follow below.

These writes retain owner/local-choice and directory guards, exact input bytes,
primary/recovery file identities and originating lifetimes. Existing files keep
their preimage in `.bak`; `.good` advances after settings publication. A missing
non-root destination uses exclusive creation with the version-two header and the
requested edit together. A missing primary with recovery bytes stays held for
repair when it is the destination. An own-list fork can read a captured valid
global recovery copy without replacing the broken global primary.

The first successful app-wide edit while its account is signed out shows one
notice per launch, with Change opening the existing account chooser. Offline
writes cannot borrow another account's send transport. Creation seeds observation
without sending, and the existing schema-two transfer and sync guards remain in
place. The two external Open actions retain their existing roles.

Headless verification passed 2,383 genuine configured-Qt/file checks, including
151 new routed-writer/list checks and 31 Peek opening-editor regression checks.
The full Debug build and link also passed. The shared core harness passed 10,403
config checks and 1,102 sync-flow checks. Native
dialog/editor behavior, Windows execution and live sync remain separate checks;
no GUI or application launch was used. The final rename window and settings,
backup and good updates remain finite checks, not a filesystem transaction.

## Local list mode and reversible forks

Settings > Purple offers Own, Follow and Stock for the window's account when
multiple accounts are loaded, including an offline app-wide source. A Stock
account keeps the control when it becomes the only account, so it can turn Work
Mode back on. The app-wide account offers On and Stock. The choice uses the shared
`SetAccountWorkMode` splice in `local.toml`; settings files and sync state stay
untouched. Apply binds the opening session, app-wide source, owner, exact inputs
and file/directory identities. A source switch, replaced input or closed origin
refuses the captured operation.

The chooser reuses the Peek countdown before entering Stock or switching between
Own and Follow; leaving Stock is immediate. App-wide source changes that enter
Normal also wait before saving. A pending Work Mode change keeps its opening
account across a displayed-account switch. See [Work Mode waits](work_mode_wait.md).

The menu verdict uses that account's effective lists and says which account owns
its members. A first Own edit names the fork and its effect on other devices;
Follow ticks name the app-wide file they edit. Stock keeps list editing and states
that Work Mode remains off here. It hides Until rows. The preset box reports Stock
for its account, or that every signed-in account is Stock. New lists and pinned
orders remain app-wide.

Delete own members is offered only for a version-2 table containing exactly one
valid integer `members` array and supported by the shared removal splice. It
removes that key and its empty header, preserves unrelated text, and restores
inheritance. The confirmation defaults to Cancel and keeps its opening destination
through Apply. Lists defined by a whole file, a title or kinds never offer this
action; Follow is the local remedy without emptying another device's list.

The file block names orphan members and members inactive under Follow, including
when the selected settings come from a last-good copy. Reload notices count
registry-scoped inactive app-wide changes, name members left unused by a removed
app-wide list, and warn when a retained definition loses all members. Open in the
notice retains the account and source snapshot. Observers seed on account/source
changes, suppress repeated notices and never rename or delete account members.

Sync choice and History previews capture the opening account's mode and name
changed own members as unused here under Follow. Sync choice Apply also retains
that opening own-file target and weak session, so changing mode or app-wide source
refuses the old preview. This adds a scope note to the existing preview, without
enabling version-two transport or a general account editor.

Desktop settings reloads retain the published Settings object while building its
successor, and the gate resolves before replacing its active resolution. The
watcher regression checks excluded chats stay hidden and muted before and after
invalidation and replacement under Own and Follow. These are single-threaded
production adapter checks, not a claim of native concurrent notification coverage.

The genuine file fixture covers transitions, lossless local writes, stale chooser
and deletion captures, forks, whole-file exclusions, reload notices, control
characters in literal list names, and opening Follow-mode preview notes. Debug
compilation/linking and the headless checks qualify code and filesystem behavior;
widget interaction, Windows execution and live sync still need separate testing.

## Reviewed legacy account transfers

Send and Share export the originating account's exact existing file. Version 1
(including an omitted version) uses `settings.toml`; version 2 uses
`account-settings.toml` and warns that older clients cannot import it. An absent
own file never borrows global bytes. Share saves a captured file through the normal
save dialog and rechecks its originating account/window before writing the export.
Exports cannot overwrite files inside the managed config directory.

Imports accept either filename and bind the message/window account, including
separate account windows. The review uses shared `AnalyzeConfigChange` facts:
per-key Used/Not used groups, pins, app-wide/list/preset/schedule losses, defined
list emptying and other loaded accounts' stranded members. Follow and Stock own
member edits remain inactive here. Global danger leads with the Work Mode/list
consequences. Cancel has initial focus; Return does not implicitly import.

The review captures exact primary/good bytes, filesystem identities, backup inputs,
local owner/source choice and loaded-account Work Mode context. A selected valid
good fallback is disclosed; unusable sources refuse. Any readable source that core
classifies as a newer schema refuses before fallback, including the destination,
global source and loaded-account context. An older good copy never downgrades it. Apply rechecks that context,
writes `.import.bak` and `.bak` from actual destination bytes, and then publishes the
exact proposed document. Missing destinations use exclusive creation.
These writes are separate operations, not a cross-file crash transaction. Source or
session changes refuse the old review; a post-publication context failure reports
that the file was already written. Modern sync reuses this authority with only a
.bak backup, after preserving the previous bytes in History.

Launch offers alone require the active account through download and Apply. Their
freshness baseline is the global file's mtime when the own file is absent. External
editor reload notices use the same impact facts after saving, since Desktop cannot
intercept an external editor's Save command. Automatic send uses only the written
file's opt-in and exact bytes. Creation, imports, History restore and signed-out
writes do not invent a send. Root markers remain in `state.toml`; non-root markers
remain in that account's `send-state.toml`, with weak-session and retry-token guards.

## Modern sync recovery boundary

Sync inspections and History listing have a separate read capability, including
missing/corrupt primaries. They create no settings or directories. A reviewed
remote adoption captures absence and exclusively creates only the selected
account's file. Its review can accept the one account directory created
by explicit setup while retaining all other bytes, identities and source choices.

History restore and Undo use captured replacement reviews with the opening
account's Work Mode choice. Modern choices also review the other device versions
that a publish would supersede. Recovery checks complete keyed/root discovery;
root Relink requires cloud proof, while Start again journals a pending-first
archive and blocks setup until exact completion. See
[modern sync and recovery](sync.md#account-specific-modern-sync-and-recovery).

## Automatic settings runner

The default-off modern automatic switch and `sync/auto.json` belong to the full
account key and its existing sync store. Root and keyed account records never
share a retry budget, ticket, saved fingerprint, in-flight owner or attention
notice. The physical account store lock is reused for the whole asynchronous
step. A caller-owned locked store passed to Apply must match the selected path.
Modern save observation is independent of legacy send-after-save preferences;
imports seed it before reload notifications without permanently suppressing a
later intentional save of the same bytes. Global/source and device Work Mode
choices remain part of the captured authority through every await. See
[sync.md](sync.md#automatic-settings-sync-on-desktop) for triggers and refusals.

List menu captures retain a refusal reason even when no writable destination was
opened. A refused tick reports that reason; it does not replace it with a file
path. Membership writes return their structured result through the UI, including
backup/publication failures and the case where settings were saved before the
context changed. Delete-members dialogs preserve the writer's error before
checking whether their original UI still survives. Stale-origin notices use only
the captured UI; they never move to another account's window. Editor writes also
distinguish an unavailable opening capture from a closed editor or changed route.

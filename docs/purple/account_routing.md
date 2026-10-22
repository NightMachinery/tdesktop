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

## Conservative limits of this batch

Version 2 is understood by the shared parser and the read-side layer, but legacy
message imports and legacy settings document export keep the version-1 boundary.
Layered imports require the later account-settings.toml document route. No highest-
version gate is advanced here merely because the core understands version 2.
Root imports and restores also refuse unparseable successors and removals of
app-wide or unknown keys, using the shared registry-derived change summary.
An explicit destination-aware destructive-change review remains later work.

Sync setup, store, publish, apply, History/Undo and fingerprint persistence now use
explicit validated originating-account files. Root-owner paths remain legacy;
nonroot accounts never fall back to them. Manual single-file export remains a
separate owner-unknown role. Account list-member writes, nonroot global editing,
list forks and the full account settings chooser remain held.
No migration-by-copy or automatic binding restoration is introduced.

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
Valid account-file manual export remains supported within the existing schema1
legacy document boundary. Layered schema2 imports/legacy sends remain refused.

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
Version2 legacy imports/exports and layered sync application remain refused;
general account editing/list forks/global-file edits stay held. The final precondition-to-rename window against a noncooperating writer remains
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
The account template is schema2, so current schema1 transport guards keep its
automatic and manual legacy sends held. The own-key Last Seen Reset described
above is separate from a general account editor/list fork or app-wide-loss review,
which remain held.

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
fingerprint and originating-session queue policy. Schema2 transport remains held.

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

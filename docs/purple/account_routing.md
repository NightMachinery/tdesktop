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

Settings > Advanced > Purple has a settings.toml ownership row and an eligible
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
layered-file creation/forks and the full account settings chooser remain held.
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

Last Seen and automatic-send settings rows read the originating account's effective
settings. The sync flag remains file-local and defaults false when absent from that
account file. Per-account writes are available only for the confirmed root owner
while that root is global. Other account rows show the existing localized Not
Available label and are disabled; queued callbacks recheck session and destination
before a guarded write. Global Premium/Work Mode rows retain their app-wide role.

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

Absent settings are read without creation and cannot initialize/publish through
this UI. File creation UI remains held. Version2 legacy imports/exports and layered
sync application remain refused; account editing/list forks/global-file edits stay
held. The final precondition-to-rename window against a noncooperating writer remains
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

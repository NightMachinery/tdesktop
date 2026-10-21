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

The existing sync/store/History implementation still uses legacy root-only
interfaces. It is explicitly held unless ownership is confirmed, the global file
is the root owner's file and the originating session is that owner. Upload no
longer selects a first or active account. Setup, publish, apply, post, launch
search and message import enforce the hold. Legacy History/Undo also refuse when
the root destination is not selected. Manual user-initiated Send remains available
in single-file mode, and otherwise reads only its originating account's path.

Non-root automatic sync, its send-state fingerprint persistence and account-
scoped History/Undo are not implemented by this batch. Account list-member writes
and editing a non-root global file are held rather than sent through unqualified
root writers. The new layered-file creation/fork operation and the full account
settings/Work Mode chooser remain later work. Existing manual Send does not claim
to update non-root per-file fingerprints. No transport, migration-by-copy or
binding restoration is introduced.

Synthetic Qt/file cases qualify the actual adapter and guarded writer in owned
temporary directories. Production-TU compilation is separate from application
linking, native UI observation, disk crash behavior and multi-process isolation.
The complete M2 client feature is not finished until the held routes above are
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

# Local message archive

The archive keeps observations separately for each signed-in account and local
profile. Payloads, body chunks, media chunks and keys are authenticated and
encrypted. SQLite indexes contain keyed identity tokens and structural counters.
Those indexes select candidates; authenticated rows establish account ownership
and message identity. This is local retention, not a Telegram cloud backup.

The storage adapter uses the committed purple-core archive contract. A private,
symbol-prefixed SQLite amalgamation is pinned under
`Telegram/ThirdParty/purple_sqlite`. `VERSION.json` records the official download,
published SHA3 and individual SHA256 hashes; `generate_prefix.py` verifies the
pristine files before regenerating the prefix map. This target does not use
QtSql, SQLite extensions or the platform SQLite library.

Each native profile directory has a random profile binding, with a private
recovery link under the archive root. Each stable account has its own store
incarnation, random data key, process lock and database. HKDF-SHA256 derives a
wrapping key from the existing random local-storage key and a separate index
key from the archive data key. Passcode changes keep the underlying local key.
A missing or unwrappable old key refuses the archive. It never creates a new key
over an existing store. A copied profile binding whose native directory identity
changed also refuses instead of silently sharing another profile's archive.

The macOS adapter excludes owned directories and files from system backups and
checks the result. Shared profile/catalogue publication is serialized across
account workers, so simultaneous sign-ins cannot lose another account entry.
Private files must belong to the current user, have no other
user permissions, and have exactly one link. Symlinks refuse. Windows has a
separate protected owner-only DACL path; its native behavior still requires
Windows qualification. Parent directories are synced after publication/removal.

All SQL preparation, binding, stepping, finalization, row/change expectations,
commit and close results are checked. An operation acknowledges retention only
after the exact core program commits. WAL, foreign keys and FULL synchronous
mode are set and read back. Every connection is admitted before configuration writes: read the full-width
format first, then the recorded fingerprint and all bounded noninternal DDL in
one read snapshot. Format 2 requires the exact core shape. Format-1 drafts and
fingerprint/DDL drift refuse as SchemaMismatch; future formats refuse ReadOnly.
There is no migration or automatic discard. Explicit account-store removal and
recreation is required to replace an incompatible draft.
Version payloads above 16 KiB use authenticated SQLite-owned body chunks. A
version above the core 4 MiB encoded limit records a retention gap instead of
claiming success. History reads use bounded pages and a revision fence; a changed
revision requires refreshing the view rather than combining different snapshots.

Media copies consume already available bytes; this adapter makes no download
requests. Media uses owned 1 MiB encrypted chunks with no arbitrary attachment
size limit or expiry. A copy must match its exact known length and whole SHA256,
be fully checked, and be durably published before its database reference becomes
Complete. Reader leases pin blobs and every chunk checks the current generation.
Extra, missing, truncated or unauthenticated chunks refuse. Failed staging and
unreferenced publication remain owned and count toward physical usage until
reconciliation or logout removes them.

Logout never waits for or depends on archive IO. Normal and forced logout hooks
revoke archive capability before upstream continues. They clear the per-login
account preferences, cancel queued work and pending deliveries, and wipe the
shared wrapping, data, index and admission keys. SQLite admission closes at
cancellation; its owner thread closes the handle after any active call returns.
No main-thread future, join or retry loop participates in logout.

Archive admission binds a random 32-byte per-login value to the sorted set of
persistent MTP key IDs across all DCs, excluding temporary and local keys. The
current main key is a wrapping selection, not the login identity. The native
account KV stores the nonce and the complete key set; the nonce survives iff
the stored set intersects the live persistent set. A disjoint live set generates
a fresh nonce. Both fields must be verified in persisted read-only TDF/KV
snapshots before opening; failed publication holds instead of falling back.

Admission version 3 has exactly eleven checked, authenticated fields: store,
key, generation, phase, slot, loginAuthority, loginAuthorityVersion,
loginAuthKeyId, loginAuthKeys, wrappedKey and loginAdmissionMac. Missing,
unknown, extra or malformed fields, unsupported versions, wrong local keys and
bad MACs refuse before any orphan decision. Earlier drafts have no implicit
migration or automatic discard; use the explicit per-account action below.

A valid record is the same login when at least one of its bound persistent IDs
is live and its nonce hash matches. Its secret is unwrapped using the old
record's authenticated set and main selection. Whenever the live set or main
selection differs, a new wrapping file is written durably first, then the whole
signed record is atomically published with its new file reference. The previous
wrap is retained. Interrupted or refused publication therefore leaves an old or
new complete valid binding, rather than a record pointing to an unwritten wrap.
Store incarnation, data key, epochs and retained versions do not change.
Unreferenced wrapping files remain owned and are removed with the store.
Rebinding runs on every open where the set or selection differs. It does not
run in an already active worker; an authorized runtime change is absorbed on
the next open/restart. This avoids an additional asynchronous writer transition.

Each login-anchor case is safe for these reasons:

- USER_MIGRATE changes the main DC without logging out. The all-DC set still
  intersects, so the nonce and archive survive and the next open rebinds them.
- Dropping a non-main key, or dropping the former main after rebind, leaves an
  intersection. The store is retained and the reduced set is published.
- Fresh login resets persistent authorization keys. A fully authenticated
  record with no live bound key is an orphan and is removed before empty Ready.
- Failed native reset followed by a new login cannot reuse stale KV: the new
  persistent set is disjoint, so selection rotates the nonce and native admission
  waits for that new pair. The authenticated old orphan is then removed.
- Nonce loss or change while any bound key is still live refuses without deleting.
  Lost KV is not proof of logout. Every other mismatch also refuses.

The persistent set is returned by mtproto/mtp_instance.h:89 and
mtproto/mtp_instance.cpp:847-854. AuthKey::keyId is exposed at
mtproto/mtproto_auth_key.h:36 and .cpp:38-40; it identifies persistent key
material, not a transport session. Fresh persistent DH material is installed by
mtproto/details/mtproto_dc_key_creator.cpp:829-839. Forced logout replaces the
MTP instance at main/main_account.cpp:543-547 and :628-637. Intro startup invokes
destroyStaleAuthorizationKeys at intro/intro_start.cpp:33 and
main/main_account.cpp:603-613 before fresh sign-in. Failed native reset can
reload stale upstream authorization; its next server rejection repeats revocation.

## Logout cleanup

A worker attempts keyless removal at logout, including when no capture worker
was running. It uses at most three attempts, 50/200 ms backoff, a 500 ms
cooperative IO budget and bounded removal pages. Persistent failure reports
pending cleanup and stops retries. Individual kernel IO calls cannot be
preempted; they never delay the main-thread logout. A durable intent is useful
for startup cleanup but is not the revocation authority. Missing intent under
total archive write failure cannot grant the old store access on re-login.
Unresolved orphan cleanup refuses Ready and retries on the next admission
opportunity. Cleanup is fenced to the old authority and exact store ID, so a
late worker cannot remove a newer login. A failed lock-file unlink is retried
only for this process's released lock with the exact native file identity.
Process-lifetime registry storage keeps self-owned worker cleanup safe during
shutdown. Quitting or switching accounts preserves the persisted authority.

The local-key and reset contract is verified in these upstream paths:
Storage::Domain::startAdded supplies the shared local key
(storage/storage_domain.cpp:77-83); Storage::Account::startAdded assigns it
(storage/storage_account.cpp:231-235). Storage::Account::reset clears _prefs
and _prefsKey (storage/storage_account.cpp:767-779), rewrites the map and
authorization data, and asynchronously removes collected old files
(storage/storage_account.cpp:820-839). It does not clear _localKey.
Both logout routes reach Account::loggedOut, session destruction and
local().reset in main/main_account.cpp. Exact current source locations are
also recorded in the external review-fix evidence.

Headless checks: `python3 purple/test_archive.py --build-dir <Debug tree>` runs
real adapters with temporary profiles. `--use-build-objects` uses the exact
production objects. On macOS these tests need backup-metadata access.
`ARCHIVE_SQLITE_TESTS=1 purple/test_config.sh` also runs the shared real-SQL suite.
Neither test opens an application window or accesses a signed-in profile.

The profile binding also includes Telegram's data-file namespace. Two `-key`
datasets in the same native `tdata` directory therefore get independent stores,
locks and wrapping contexts. A private native-directory recovery link survives
loss of just the profile marker without choosing a fresh empty archive.

## Windows durability

On Windows, regular-file flushing uses the write-access QSaveFile handle.
Directory metadata durability uses a checked writable volume handle, never a
directory FlushFileBuffers call. Windows requires administrative volume access
for this documented barrier; unavailable access refuses archive operations and
leaves a visible held state. The client does not request elevation or silently
claim a directory flush succeeded. Native Windows runtime qualification remains
pending. References: [FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)
and [volume handles](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew).

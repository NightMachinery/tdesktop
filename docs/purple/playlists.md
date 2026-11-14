# Playlists

The Desktop build includes the shared playlist foundation from purple-core:
strict library and local-state codecs, edits and undo, deterministic merge,
bounded gzip records, record discovery, verified cover checks and complete
sync-step planning. Its contract and focused tests are described in
[the core playlist documentation](../../Telegram/ThirdParty/purple_core/docs/playlists.md).

These sources alone do not expose playlists in the client. Menus, playlist
queues, downloads and sync transport remain to be implemented.
Sharing is deferred, and changing the sync chat does not clean records from the
old chat.

The gzip codec uses the existing `desktop-app::external_zlib` dependency.
Playlist sources are registered in `Telegram/cmake/purple.cmake`, alongside
the other shared Purple sources.

Private music listening is connected independently of the playlist UI. It is on
by default for all music playback, with an app-wide switch under Settings >
Purple > Media playback. See [media playback](media_playback.md).

## Account-local storage

`DesktopPlaylistStore` stores `library.json` and `local.json` under the account's
`playlists` directory from `ResolveAccountFiles`. It accepts a captured account
routing snapshot and a mandatory, thread-safe route validator. Callers must
revoke that validator when the account closes or the route changes. Opening
also requires a readable, valid root-owner record matching the captured owner.
The store then checks directory identities and the captured local-choice and
owner files throughout writes.

One worker thread owns each store, and a per-account lock prevents competing
writers. Reads return a revision and exact file snapshots. A stale revision,
changed file, changed route, symlink, unsafe permissions or reentrant operation
refuses the write. Missing files yield the core defaults; unreadable, invalid
and newer files remain untouched and block writes. Local reads use the observed
file size rather than a fixed record-size cap.

Writes use the shared durable atomic writer with exact readback and file and
directory barriers. A changing merge saves the previous library bytes as
`library.json.bak` first. The new library must be confirmed before its local
state or memo is saved. A first memo also requires a materialized library,
including an empty one. A failed state write can leave a confirmed new library
with the previous memo, so the next read safely retries unfinished work.
Results distinguish confirmed writes from files that may have changed before a
durability failure; callers must reload after any attempted write.

Callers derive edits from the returned typed state, preserving its unknown
fields, and serialize local edits and merge results through the same store.
The storage component performs no listing, upload, pruning or account login.
`cases-playlists-store` exercises real temporary files, the real codecs and
durability helpers, with controlled routing changes and write failures.

## Account service

`ReadAccountPlaylists` and `EditAccountPlaylists` lazily create an account-owned
`crl::object_on_thread` worker. Reads and edits run serially on that thread,
including opening and releasing the per-account store lock. Each edit reloads
the current library before applying a typed shared-core operation, preserves
unknown local fields and the earliest unsynced age, then reloads after an
attempted write. Results keep core edit errors and partial durable-write
outcomes separate from the current snapshot.

Only the main thread captures the account routing and touches session objects.
Routing changes, ordinary or forced logout, and session destruction revoke an
atomic token before retiring the worker. Queued work checks that token through
the store's durable-write guards. Already committed files remain committed;
revocation cannot undo accepted writes. Main-thread completions recheck the
route, and destroyed sessions receive no late callback. A retiring worker may
briefly retain its lock, so a new route can report Busy until it has closed.

The service performs no automatic account-file access at startup. The playlist
UI will request a read when needed. `cases-playlists-service` exercises the
production service body with controlled account and routing ports, real CRL
threads, temporary files, shared-core edits and the durable store.

## Song resolution

`DesktopPlaylistResolver` reads sources for the captured session and account.
It checks loaded messages first, then batches up to 100 sources through the
shared `SyncRequestSender`. Self, user and basic-group references use
`messages.getMessages`; channels use `channels.getMessages` and their access
hash. An unknown channel can first use its saved public username hint, but the
resolved numeric channel must match the stored channel. A reassigned username
cannot redirect a song. Missing hints or access report No access; callers can
retry when the channel becomes known on that device.

Every successful source must be the expected document, a regular server song
in the message's main media, and permitted by the existing save policy.
Definitive Deleted, No access, Changed or Restricted results try the saved
fallbacks in order. Network failures stop that row until the next explicit
resolution; flood waits remain with the shared request scheduler. Resolving a
fallback never relinks the stored song. Invalid or incomplete response shapes
cannot establish that a song was deleted.

The resolution queue preserves row order, yields between chunks even for
cached rows, and drops late or duplicate completions. It returns source
references for playback through the stock message player and file-reference
refresh path. Results are memory-only. `cases-playlists-resolver` exercises the
production queue and channel-identity check with controlled cache/read/defer
ports; the real Telegram adapter is syntax-compiled. Live RPC behavior and
playlist UI/player integration are not yet qualified.

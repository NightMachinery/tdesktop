# Playlists

The Desktop build includes the shared playlist foundation from purple-core:
strict library and local-state codecs, edits and undo, deterministic merge,
bounded gzip records, record discovery, verified cover checks and complete
sync-step planning. Its contract and focused tests are described in
[the core playlist documentation](../../Telegram/ThirdParty/purple_core/docs/playlists.md).

These sources alone do not expose playlists in the client. Song resolution,
menus, playlist queues, downloads, sync transport and the account service that
connects them remain to be implemented. Sharing is deferred, and changing the
sync chat does not clean records from the old chat.

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

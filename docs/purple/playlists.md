# Playlists

The Desktop build includes the shared playlist foundation from purple-core:
strict library and local-state codecs, edits and undo, deterministic merge,
bounded gzip records, record discovery, verified cover checks and complete
sync-step planning. Its contract and focused tests are described in
[the core playlist documentation](../../Telegram/ThirdParty/purple_core/docs/playlists.md).

These sources alone do not expose playlists in the client. Account-local
storage, song resolution, menus, playlist queues, downloads and sync transport
remain to be connected. Sharing is deferred, and changing the sync chat does
not clean records from the old chat.

The gzip codec uses the existing `desktop-app::external_zlib` dependency.
Playlist sources are registered in `Telegram/cmake/purple.cmake`, alongside
the other shared Purple sources.

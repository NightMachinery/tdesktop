# Purple backlog

Current feature requests and follow-up checks for the desktop and Android
clients. Historical Android verification notes remain in [todo.md](todo.md).

## Open

- **Android notification preview exceptions:** shared core and desktop support
  are done. Android preview-safe integration is committed and pushed in
  `9363b25f`; source-only JNI/Java syntax checks passed. Full app build, signing,
  and device runtime verification are pending.
- **Pinned music menu:** desktop prefetches pinned songs into Telegram's cache
  from the open-chat menu, with one music neighbor on each side and album
  expansion by default. The desktop source is committed and pushed, but the
  installed daily-use app is still older. The Android action remains pending. See
  [pinned_music.md](pinned_music.md).
- **Per-chat download cache setting:** expose it from the chat menu. Semantics
  need clarification. Desktop has an automatic media download override; Android
  has no per-chat override.
- **Last Seen Peek:** the checkbox now skips confirmation and Peek is enabled by
  default in both clients. Android APK `ef73312d0e50c0b561fb264ac4b3556d9835be7a`
  includes the checkbox, badge refresh source, and Persian-layout passcode
  mapping. An account-backed test of the eligible Peek UI is still needed. The
  badge refresh runtime behavior has not been independently verified; mute
  behavior already matches normal behavior.
- **Contacts filter:** the empty state is misleading and needs correction.
- **Android passcode verification:** Persian-layout passcode input is
  implemented in both clients and included in the delivered release-signed APK
  above, built with core `e7bf8544`. Device unlock runtime behavior has not been
  independently verified.

## Retired

- Translation bar and video-ad verification are retired; see the recorded
  outcomes in [todo.md](todo.md). Do not reopen these requests.

# Purple backlog

Current feature requests and follow-up checks for the desktop and Android
clients. Historical Android verification notes remain in [todo.md](todo.md).

## Open

- **Android notification preview exceptions:** shared core and desktop support
  are done. Android preview-safe integration is included in the release from
  source `9b908110`. The APK installs and launches, but preview-suppression
  behavior remains unverified.
- **Pinned music menu:** Android ships the chat-menu action and dialog in the
  release from source `9b908110` with core `e7bf8544`. The dialog was observed
  with one song before and after, and whole-album inclusion checked by default;
  full pin enumeration, neighbor/album resolution, and cache download behavior
  still need runtime verification. Desktop source is built and staged, but the
  installed daily-use app remains older. See [pinned_music.md](pinned_music.md).
- **Per-chat download cache setting:** expose it from the chat menu. Semantics
  need clarification. Desktop has an automatic media download override; Android
  has no per-chat override.
- **Last Seen Peek:** the checkbox now skips confirmation and Peek is enabled by
  default in both clients. The APK from Android source `9b908110` installs and
  launches, but an account-backed eligible Peek and badge-refresh behavior
  remain unverified. Mute behavior already matches normal behavior.
- **Android passcode verification:** Persian-layout passcode input is
  implemented in both clients and included in the release from Android source
  `9b908110`, built with core `e7bf8544`. Device unlock runtime behavior still
  needs focused verification.

## Retired

- Translation bar and video-ad verification are retired; see the recorded
  outcomes in [todo.md](todo.md). Do not reopen these requests.

# Purple backlog

Current feature requests and follow-up checks for the desktop and Android
clients. Historical Android verification notes remain in [todo.md](todo.md).

## Open

- **Android notification preview exceptions:** shared core and desktop support
  are done. Android preview-safe integration is included in the release from
  source `5e428532` with core `e7bf8544`. The APK installs and launches, but
  preview-suppression behavior remains unverified.
- **Pinned music menu:** Android ships the chat-menu action and dialog in the
  release from source `5e428532` with core `e7bf8544`. Its dialog defaults
  were observed on `9b908110` and carried unchanged into `5e428532`: one song
  before and after, with whole-album inclusion checked. Android pin enumeration,
  neighbor/album resolution, and cache download behavior still need runtime
  verification. The desktop change is installed in the daily-use app and passed
  an app launch smoke test, but the pinned music menu and cache behavior still
  need runtime verification. See [pinned_music.md](pinned_music.md).
- **Per-chat download cache setting:** expose it from the chat menu. Semantics
  need clarification. Desktop has a per-peer automatic media-download override.
  Android has per-chat Keep Media retention exceptions, but no per-chat
  automatic-download override; decide which behavior belongs in the menu.
- **Last Seen Peek:** the checkbox now skips confirmation and Peek is enabled by
  default in both clients. The APK from Android source `5e428532` installs and
  launches, but eligible Peek and badge-refresh behavior remain unverified.
  Mute behavior already matches normal behavior.
- **Android passcode verification:** Persian-layout passcode input is
  implemented in both clients and included in the release from Android source
  `5e428532`, built with core `e7bf8544`. Desktop passcode-lock launch was
  tested; Android device unlock runtime behavior still needs focused
  verification.

## Retired

- Translation bar and video-ad verification are retired; see the recorded
  outcomes in [todo.md](todo.md). Do not reopen these requests.

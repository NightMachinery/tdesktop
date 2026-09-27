# Purple backlog

Current feature requests and follow-up checks for the desktop and Android
clients. Historical Android verification notes remain in [todo.md](todo.md).

## Open

- **Android notification preview exceptions:** shared core and desktop support
  are done; Android notification formatting is pending.
- **Pinned music menu:** add “download all pinned songs” to both clients. Default
  neighbor counts to `M=N=1` and enable album expansion by default.
- **Per-chat download cache setting:** expose it from the chat menu. Semantics
  need clarification. Desktop has an automatic media download override; Android
  has no per-chat override.
- **Last Seen Peek:** the checkbox now skips confirmation and Peek is enabled by
  default in both clients. The current Android APK at `85ec9cff` includes the
  checkbox but predates the Persian-layout passcode fix. An account-backed test
  of the eligible Peek UI is still needed. Android badge refresh on open and
  close transitions is committed in `89376627` but awaits an APK; mute behavior
  already matches normal behavior.
- **Contacts filter:** the empty state is misleading and needs correction.
- **Android passcode package:** Persian-layout passcode input is implemented in
  both clients. A signed Android APK for `89376627`, including the passcode fix,
  is pending overnight.

## Retired

- Translation bar and video-ad verification are retired; see the recorded
  outcomes in [todo.md](todo.md). Do not reopen these requests.

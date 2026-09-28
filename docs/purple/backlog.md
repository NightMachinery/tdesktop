# Purple backlog

Current feature requests and follow-up checks for the desktop and Android
clients. Historical Android verification notes remain in [todo.md](todo.md).

## Open

- **Android notification preview exceptions:** shared core and desktop support
  are done. Android preview-safe integration is included in the release from
  source `5e428532` with core `e7bf8544`. A rich-message path in that APK could
  show message text despite preview suppression. Android source `d3844efb`
  fixes that bypass. Source `cbd5f02b` also hides self-destructing-media
  captions in pinned, short, and full notification previews. These fixes await
  a new signed APK and runtime check. Other preview-suppression behavior
  remains unverified.
- **Pinned music menu:** Android ships the chat-menu action and dialog in the
  release from source `5e428532` with core `e7bf8544`. Its dialog defaults
  were observed on `9b908110` and carried unchanged into `5e428532`: one song
  before and after, with whole-album inclusion checked. A large-channel Android
  run reached the pinned songs but exposed 143 failed cache transfers reported
  as `RETRY_LIMIT` while the UI only said they were queued. Bounded transfers,
  retries, and a progress bar are committed in Android source `0196012c`; an
  updated APK and runtime retest are pending. The desktop change is installed
  in the daily-use app and passed an app launch smoke test, but its menu and
  cache behavior still need runtime verification. See
  [pinned_music.md](pinned_music.md).
- **Per-chat Keep Media:** the user clarified that the chat-menu setting means
  cache retention, not automatic download. Android already has per-chat
  retention exceptions; its chat-menu display and editor are committed in
  Android source `1593b8d1` and await an updated APK. Desktop currently exposes
  global cache retention and a separate per-peer automatic-download override,
  but no matching per-chat retention setting. Its cache records a media key,
  category tag, and access time without a chat identifier. A desktop per-chat
  retention rule therefore needs ownership tracking, including a policy for
  media shared across chats, before the menu can show an enforceable value.
  See [keep_media.md](keep_media.md) for the implementation contract.
- **Last Seen Peek:** the checkbox now skips confirmation and Peek is enabled by
  default in both clients. The APK from Android source `5e428532` installs and
  launches, but eligible Peek and badge-refresh behavior remain unverified.
  Mute behavior already matches normal behavior.
- **Android Contacts Last Seen filter:** the signed APK from source `5e428532`
  shows “Visible or peekable last seen” in Contacts More. Turning it on showed
  the active-filter empty state, and turning it off restored the same contact.
  The test account had no eligible contact, so inclusion of a visible or
  peekable person still needs runtime verification.

## Verified

- **Persian-layout passcode:** the Android APK from source `5e428532`, built
  with core `e7bf8544`, unlocked a text passcode set as `qwer` with Persian
  keyboard input `ضصثق`. The exact ASCII passcode also unlocked it. The test
  account remained logged in after Passcode Lock was turned off. Desktop
  passcode-lock launch was also tested. See [passcode.md](passcode.md).

## Retired

- Translation bar and video-ad verification are retired; see the recorded
  outcomes in [todo.md](todo.md). Do not reopen these requests.

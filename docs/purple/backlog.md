# Purple backlog

Current feature requests and follow-up checks for the desktop and Android
clients. Historical Android verification notes remain in [todo.md](todo.md).

## Open

- **Account-backed settings and playlist sync:** the design in
  [account_sync_plan.md](account_sync_plan.md) uses opt-in records in each
  account's Saved Messages, with config first and playlist metadata later.
  Shared core now constructs config version ancestry, classifies incoming
  heads, canonicalizes strict JSON, validates and builds config records, and
  checks versioned device-local sync state for exact read-back and clone/rewind
  signals. It can also acknowledge an applied remote config without creating
  a new version. A deterministic three-device core
  simulation covers delayed discovery, edit and repost modes, failure retry,
  conflicts, and restart recovery. Desktop has an unused durable local store
  for staged config records and state, including exact read-back confirmation
  and restart cleanup. Android local storage and version-aware confirmation,
  transport, and UI are still unimplemented.
  A disposable-account test must establish whether media can be replaced
  in place between clients before choosing that transport over bounded
  reposts. Android source `6df36b1d` preserves a dedicated pre-import backup;
  `a950e13c` aligns settings size and Saved Messages search limits with
  desktop. Android `8b07614c` and desktop `f7f536d165` now record a sent
  fingerprint and suppress self-offers only after Telegram confirms a server
  message id. The signed `8b07614c` APK upgraded the retained test account
  and was delivered privately on 2026-09-29. The desktop change passed a full
  optimized build and isolated launch but is not in the installed app yet.
  Live receipt behavior, import-backup and 4 MiB boundaries, persistent
  incoming offers, and playlists remain open. Android's generic Retry after a
  failed send still bypasses Purple's receipt callback.
- **Android notification preview exceptions:** shared core and desktop support
  are done. Android preview-safe integration is included in the release from
  source `5e428532` with core `e7bf8544`. A rich-message path in that APK could
  show message text despite preview suppression. Android source `d3844efb`
  fixes that bypass. Source `cbd5f02b` also hides self-destructing-media
  captions in pinned, short, and full notification previews. Source `d5774ea9`
  keeps a forwarded sender name hidden in a non-exempt verification
  notification while passcode-locked. These fixes are included in the signed
  Android APK from source `32c1ee0c`, which installed and launched with the
  test account intact. The notification privacy paths still need runtime
  checks.
- **Pinned music menu:** Android ships the chat-menu action and dialog in the
  release from source `5e428532` with core `e7bf8544`. Its dialog defaults
  were observed on `9b908110` and carried unchanged into `5e428532`: one song
  before and after, with whole-album inclusion checked. A large-channel Android
  run reached the pinned songs but exposed 143 failed cache transfers reported
  as `RETRY_LIMIT` while the UI only said they were queued. Bounded transfers,
  retries, and a progress bar are committed in Android source `0196012c`.
  A source audit also found that a download already in progress could be
  reported as failed when FileLoader finished it outside the cache directory.
  Android source `6fdb061a` checks the actual completed file in FileLoader's
  managed locations. Source `bce320af` clarifies the Retry label when a failed
  search and failed songs are retried together. A large-channel run in the
  earlier candidate reached 339/339, but a reported song still showed a
  download arrow: the completed file existed only in the general app cache.
  Android source `1f819cff` stores new songs in the chat-recognized media
  location, promotes complete legacy cache files, and records their chat owner.
  Source `32c1ee0c` adds a per-file status and Retry dialog when the progress
  bar is tapped. Its signed APK installed without losing the test account.
  After a full run, 339 distinct selected songs mapped to 339 nonempty normal
  media files, and the reported song no longer showed a download arrow. Moving
  one test song aside changed progress to 338/339 with an explicit missing-file
  reason; Retry restored it and progress returned to 339/339. Android source
  `8818448c` adds the searchable full-screen file list, separate play/pause and
  row jump, plus a chat-menu shortcut to that chat's storage sheet. The signed
  test build passed emulator checks including a filtered jump into older
  history. The final signed APK was built from clean `8818448c`, installed
  without losing the test account, and delivered privately. Migrated-group
  and forum-topic jumps still need a suitable test account. The desktop
  change is installed in the daily-use app and passed a launch smoke test, but
  its menu and cache behavior still need runtime verification.
  See [pinned_music.md](pinned_music.md).
- **Per-chat Keep Media:** the chat-menu request means local cache retention,
  not automatic download. Android's per-chat retention editor is committed in
  source `1593b8d1`. The signed `bce320af` candidate showed the effective
  default duration in the chat menu, changed to a per-chat Forever exception,
  and returned to the default after deleting the exception. That behavior is
  included in the signed `32c1ee0c` APK. Long-term cache expiry still needs
  runtime verification. File-to-chat records apply the exception to attributed
  downloads; unknown or shared files use the general policy, and the size
  limit may evict finite-retention files early. Desktop remains global-only
  for cache retention; per-chat desktop retention is deferred.
  See [keep_media.md](keep_media.md) for the current scope.
- **Last Seen Peek:** the checkbox skips confirmation and Peek is enabled by
  default in both clients. Android source `b2f416a5` refreshes status links
  when the setting changes and journals the original privacy rules before a
  Peek, then verifies their restoration after the deadline or reconnection.
  The signed APK from source `32c1ee0c` installs and launches, but eligible
  Peek and restoration behavior remain unverified on a live contact. Desktop
  source `c894b72c07` similarly journals and verifies its privacy restoration.
  Its optimized app built and installed successfully. An unsandboxed launch
  with a disposable workdir reached normal startup with Debug off. A separate
  launch with its normal profile reached the established local passcode screen,
  confirming it did not start a fresh login; chats and eligible Peek behavior
  remain unverified behind the lock. Mute behavior already matches normal
  behavior.
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

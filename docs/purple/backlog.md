# Purple backlog

Current feature requests and follow-up checks for the desktop and Android
clients. Historical Android verification notes remain in [todo.md](todo.md).

## Open

- **Android notification preview exceptions:** shared core and desktop support
  are done. Android preview-safe integration is included in the release from
  source `5e428532` with core `e7bf8544`. A rich-message path in that APK could
  show message text despite preview suppression. Android source `d3844efb`
  fixes that bypass. Source `cbd5f02b` also hides self-destructing-media
  captions in pinned, short, and full notification previews. Source `d5774ea9`
  keeps a forwarded sender name hidden in a non-exempt verification
  notification while passcode-locked. The signed Android APK from source
  `3559ef67` installed and launched with the test account intact, but the
  notification privacy paths still need runtime checks.
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
  search and failed songs are retried together. On a disposable Android account,
  the signed `bce320af` candidate showed the requested menu/dialog and completed
  a large-channel run at 339/339 with no failed tracks, at most two active
  transfers, and recent complete MP3 files in the app cache. Pause/resume and
  leaving/reopening the chat worked. Retry could not be tested because nothing
  failed. The signed `3559ef67` APK installed and showed both chat-menu actions
  with the account intact. The desktop change is installed in the daily-use
  app and passed an app launch smoke test, but its menu and cache behavior
  still need runtime verification.
  See [pinned_music.md](pinned_music.md).
- **Per-chat Keep Media:** the chat-menu request means local cache retention,
  not automatic download. Android's per-chat retention editor is committed in
  source `1593b8d1`. The signed `bce320af` candidate showed the effective
  default duration in the chat menu, changed to a per-chat Forever exception,
  and returned to the default after deleting the exception. Long-term cache
  expiry still needs runtime verification. Desktop remains global-only for
  cache retention; per-chat desktop retention is deferred.
  See [keep_media.md](keep_media.md) for the current scope.
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

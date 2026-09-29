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
  reason; Retry restored it and progress returned to 339/339. The desktop
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
  with a disposable workdir reached normal startup with Debug off; the
  established account and eligible Peek behavior remain unverified. A strict
  signing check still reports an untrusted local certificate, although the
  installed app launches. Mute behavior already matches normal behavior.
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

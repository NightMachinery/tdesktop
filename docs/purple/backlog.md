# Purple backlog

Current feature requests and follow-up checks for the desktop and Android
clients. Historical Android verification notes remain in [todo.md](todo.md).

## Open

- **Account-backed settings and playlist sync:** the design in
  [account_sync_plan.md](account_sync_plan.md) uses opt-in records in one
  selected account's Saved Messages per device, with config first and playlist
  metadata later. The proposed UI has one Sync across devices entry, persistent
  updates to review, local History and Undo, and explicit conflict choices.
  Shared core now constructs config version ancestry, classifies incoming
  heads, canonicalizes strict JSON, validates and builds config records, and
  checks versioned device-local sync state for exact read-back and clone/rewind
  signals. It also records confirmed own message IDs for exact-read-back
  cleanup, orders time-prefixed sync-space IDs, and acknowledges an applied
  remote config without creating a new version. A deterministic three-device
  core simulation covers delayed discovery, edit and repost modes, failure retry,
  conflicts, and restart recovery. Desktop has a durable local store
  for staged config records and state, including exact read-back confirmation,
  atomic own-message ledger persistence, and restart cleanup. A
  desktop scanner can page the account's full Saved Messages history without
  sending anything, and reports an incomplete result after failure or cancel.
  A desktop reader re-fetches every candidate document, caps its
  download at 4 MiB, and reports newer, changed, missing, or unreadable records
  instead of treating them as an empty account. A coordinator ties scan and
  read to one selected session, returning Complete only when both finish. The
  desktop Advanced settings box requires an explicit account choice on
  multi-account installs, shows scan progress, and reviews the settings
  records it finds. It offers one manual action per check: Publish settings,
  Join sync, Review update, Choose settings, Publish changes, or Finish
  sending, with a change summary and line diff before any local write, a
  fresh scan and cloud disclosure before any post, and History, Restore and
  Undo for every replaced file. Still open on desktop: background checks,
  automatic publish after edits, edit in place, retirement of superseded
  records, and a live pass of the fix for the unknown-outcome bug the
  2026-10-01 live run found (below).
  Android JNI can
  initialize local state bound to the active Telegram user, reserve a canonical
  own config record with its pending key, confirm an exact staged read-back
  with its message ID, and format or compare time-ordered space IDs. Its
  account preference token is keyed by the active user ID; both reservation
  paths refuse a missing or mismatched token.
  First setup needs a complete Saved Messages history scan before it can
  declare the account empty; search with a watermark alone can miss a delayed
  older entry.
  Android now has the same manual flow in Settings → Purple → Sync across
  devices (source `2af13dd5`): a Saved Messages transport, join, apply,
  publish, History, Restore and Undo, decided by the same purple-core code,
  for the account the settings screen belongs to. Its durable local store
  locks the sync directory and pauses on ambiguous crash recovery rather than
  sending uncertain data. An emulator run on 2026-09-30 passed first publish,
  private record downloads, a second install joining through Adopt, Publish
  changes, Update with Apply and Undo, Conflict with each side picked, a post
  interrupted by force-stop and resent without a duplicate, Cancel, and
  History restore and pruning. It also found and fixed a check that could
  miss a post Telegram resent mid-scan (Android `108c2c24`, desktop
  `6f4c252ddb`). The signed APK from source `a12cbbaa` was delivered
  privately. Not reachable there: an account switch mid-check (one account)
  and a remote pick that leaves another head (host-tested). Still open on
  both clients: background checks, automatic publish, and retirement of
  superseded records.
  A disposable-account test must establish whether media can be replaced
  in place between clients before choosing that transport over bounded
  reposts. Android source `6df36b1d` preserves a dedicated pre-import backup;
  `a950e13c` aligns settings size and Saved Messages search limits with
  desktop. Android `8b07614c` and desktop `f7f536d165` now record a sent
  fingerprint and suppress self-offers only after Telegram confirms a server
  message id. The signed `8b07614c` APK upgraded the retained test account
  and was delivered privately on 2026-09-29. The desktop change passed a full
  optimized build and isolated launch but is not in the installed app yet. It
  also never worked: the uploader dropped the receipt of every document, so
  desktop recorded no legacy send until the 2026-10-01 fix below.
  An emulator run on 2026-09-30, on the installed `b0bd74c7` build, verified
  the Android import backup: an import keeps the exact previous bytes in
  `settings.toml.import.bak`, a second import replaces them, an editor save
  leaves them alone, and an import that cannot write that backup stops with
  `settings.toml` unchanged. The same run verified the size limits. Exactly
  4 MiB is accepted and 4 MiB + 1 refused by the editor's save and by import,
  both from a file and from Saved Messages, where a larger `settings.toml`
  gets no Import button and no launch offer. The Sync across devices check
  calls a file over 256 KiB unusable and accepts one of exactly 256 KiB.
  The run also found that Import from a file failed with a permission error
  for a file another app had put in Download when it was picked through the
  device's storage root. Android `1c54ff47` reads such a file through the
  picker's grant and deletes the temporary copy afterwards (the old code also
  left every picked copy in the external cache). The run also found that an
  import that failed on the import backup had already refreshed
  `settings.toml.bak` with the current file. Android `dca9a039` writes the
  import backup first, so that failure leaves `settings.toml.bak` alone, and
  a host test covers the order. A second emulator run, on the installed
  `dca9a039` build, verified both fixes. Through the device's storage root,
  a Download file imports, exactly 4 MiB is accepted and 4 MiB + 1 refused,
  and no copy stays in the external cache; the Downloads root still works.
  With the import backup blocked, either at its temporary file or at the
  backup itself, the import stops and `settings.toml` and
  `settings.toml.bak` keep the same SHA-256 and mtime. Persistent incoming
  offers and playlists remain open.
  Android source `4881e7c0`
  restores Purple's receipt callback when Telegram retries a failed settings
  document from Purple's staging directory in Saved Messages. A live emulator test
  on 2026-09-30 found that Telegram's automatic resend at app start, after the
  app was closed during a send, still recorded no receipt, so Purple later
  offered the device its own file for import. Android `5c687693` also accepts
  that resend, and `7fc91602` prunes empty staging directories. The signed
  `7fc91602` APK recorded the sent fingerprint and offer watermark on the
  emulator's startup resend, made no self-import offer afterwards, and was
  delivered privately. Android `8d6bc3a8` moves that staging from Telegram's
  cache to the app's external files directory
  (`Android/data/<package>/files/purple-sync/`), because Telegram moves a
  sent cache attachment into Telegram Documents, which is shared storage on
  Android 10 and lower. The receipt accepts copies staged in either place.
  The 2026-09-30 emulator run verified a send, a restart resend and a manual
  Retry of a failed send from the new directory (the failure was forced by
  making the staged copy unreadable, then restoring it), and delivery of a
  copy staged by the older build after an in-place upgrade.
  A live cross-client run on 2026-10-01 drove an isolated desktop profile
  (built from `ef16c54e3b`) against the emulator's installed Android build
  `95171e565`, on the same disposable account. Both clients' settings files
  were restored afterwards.
  - Passed: the desktop's first check named the Android device and showed a
    summary and line diff. Joining wrote the chosen record's exact bytes after
    a History entry, and Undo and Restore put back exact bytes.
  - Passed: an update each way. The receiving device's review named the
    sender, Apply kept History first and wrote the sender's bytes, and Undo
    restored the previous bytes.
  - Passed: a conflict picked once per side. Picking the other device's
    version wrote it and posted nothing. Picking this device's own settings
    showed the disclosure, posted once and left the file unchanged.
  - Passed: the still-sending refusal. With the record's upload held at a
    local test proxy, Publish changes stayed queued. After the box was closed
    and reopened, Check offered Finish sending, and Finish sending refused
    with the still-sending text. Once the copy arrived, the next check
    confirmed it.
  - Passed: two copies that arrived while a check was stalled or mid-scan were
    each confirmed by Finish sending without a second post.
  - Passed: an Android check counted exactly one record per publish across
    the run, 22 in all.
  - Passed: the desktop log holds only counts and paths, no settings text.
    The emulator log holds the account's numeric user ID, which the design
    already logs.
  - Passed: no sync record appeared in the desktop download folder.
  - Bug: every desktop Publish settings or Publish changes that finished with
    the box open reported "Outcome unknown" within about four seconds,
    although the post had arrived. This happened 4 of 4 times, on a working
    network. The stage stays pending, and the next check's Finish sending
    confirms it without a second post. During one of these posts the proxy
    log showed no new connection, because no read-back ever started.
    The cause was in the upload hand-off: `Uploader::finishFront` passed the
    send receipt on for photos only. A record is a document, so its receipt
    was destroyed as the upload finished and reported no message id before
    the server answered, and `SyncConfigPost::OnReceipt` correctly called
    that an unknown outcome. The same loss made the manual legacy send say
    "Could not confirm settings send" and kept both legacy sends from
    recording what they sent. The document branch now passes the receipt
    on; a live pass has to confirm it.
  - Smaller findings. Desktop drops the Undo last update row when the box
    closes, because Undo lives in the box; History Restore still works, and
    Android keeps its row. The box's Close button is clipped to "lose", and
    Cancel check shows while no check runs. After a Restore, "stays on this
    device until you publish it" sits next to "Up to date".
  - Not reachable with two devices: Android's apply-then-share bulletin.
    Picking another device's version shares only when other heads remain, and
    "Use and share" with this device's own settings writes no file. A failed
    (red) send also could not be produced through a proxy outage, because
    Telegram keeps retrying.
- **Android notification preview exceptions:** shared core and desktop support
  are done. Android preview-safe integration is included in the release from
  source `5e428532` with core `e7bf8544`. A rich-message path in that APK could
  show message text despite preview suppression. Android source `d3844efb`
  fixes that bypass. Source `cbd5f02b` also hides self-destructing-media
  captions in pinned, short, and full notification previews. Source `d5774ea9`
  keeps a forwarded sender name hidden in a non-exempt verification
  notification while passcode-locked. These fixes are included in the signed
  Android APK from source `32c1ee0c`, which installed and launched with the
  test account intact. An emulator run on 2026-09-30, on the installed
  `b0bd74c7` build, sent inbound bot messages with `tsend` and read the
  posted notifications with `dumpsys notification --noredact` and the
  notification shade.
  - With previews off for private chats and `preview_always = []`, a text
    message showed only "<bot> sent you a message" in the full preview and
    "Message" in the short one. A rich message was hidden the same way, which
    confirms the `d3844efb` fix.
  - With the default exemptions (`MAGIC_BOTS`), the same bot's plain and rich
    messages showed their text with previews off.
  - While passcode-locked, a non-exempt message showed only "You have a new
    message". An exempt bot's message still showed its text.
  - Not reachable with a bot: pinned-message previews, self-destructing
    captions, and the forwarded sender in the Verification Codes chat. They
    need a second account, or a bot pin through the Bot API, and a real
    forwarded code message.
  - Upstream behavior, not a leak: the full preview of an allowed rich
    message is the generic "sent you a message" line, because only the short
    builder handles rich messages.
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
  without losing the test account, and delivered privately. An emulator run
  on 2026-09-30, on the installed `b0bd74c7` build and in groups whose only
  member is the test account, verified the migrated-group and forum-topic
  cases. In a basic group migrated to a supergroup, the action found the pins
  in both the old and the new peer, and each file-list row jumped to its own
  message in either peer. In a forum topic, the pinned search and the neighbor
  search both stayed inside the topic, and row jumps landed in the topic.
  The desktop
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
  Peek and restoration behavior remain unverified on a live contact. A check
  on 2026-09-30 found the test account's only contact shows "last seen
  recently" without the by-me mark. That status is coarse by the contact's
  own rules, so the contact is not peekable. The test account's own Last Seen
  rule is Nobody. Reaching Peek needs a second disposable account, added as a
  contact, whose own Last Seen rule shows it to the test account. Desktop
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
  peekable person still needs runtime verification. A repeat on the
  `b0bd74c7` build on 2026-09-30 gave the same result. The second disposable
  contact that would reach Peek (above) reaches this filter too.

## Verified

- **Persian-layout passcode:** the Android APK from source `5e428532`, built
  with core `e7bf8544`, unlocked a text passcode set as `qwer` with Persian
  keyboard input `ضصثق`. The exact ASCII passcode also unlocked it. The test
  account remained logged in after Passcode Lock was turned off. Desktop
  passcode-lock launch was also tested. See [passcode.md](passcode.md).

## Retired

- Translation bar and video-ad verification are retired; see the recorded
  outcomes in [todo.md](todo.md). Do not reopen these requests.

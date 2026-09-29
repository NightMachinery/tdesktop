# Pinned music cache prefetch

The open-chat menu offers **Download all pinned songs**. The dialog defaults to
one music message before and one after each pinned song. Each count accepts
0–20. **Include the whole audio album** is enabled by default. Download uses
Telegram's managed media cache; it does not export files to Downloads.

The action searches every page of pinned messages in the open chat or forum
topic and, for a migrated group, its old chat. Only accessible pinned song
messages are anchors. Neighbor searches use Telegram's music filter and
message order, scoped to the topic when one is open. Album expansion requests
100 messages around the anchor from chat history or topic replies, so members
absent from the loaded timeline can be included. Telegram Desktop's group
model caps an album at 10 items, so the 100-message request comfortably covers
ordinary contiguous albums. Each audio message is queued once by peer and
message ID. If the search finds no pinned songs, no transfer starts.

Android ships the menu action and dialog in the release from source
`5e428532` with core `e7bf8544`. The dialog defaults were observed on source
`9b908110` and carried unchanged into `5e428532`: one song before and after,
with whole-album inclusion checked. A run in a large test channel showed 115
pinned messages and hundreds of cache files, but 143 transfers ended with
`RETRY_LIMIT`; the old toast reported only how many songs were queued. Source
work now limits concurrency, backs off and retries these failures, and adds
visible completion and failure counts in Android source `0196012c`. A source
audit found one additional case: an already-running ordinary download could
finish in FileLoader's media directory while the pinned-music job checked only
its cache directory. That completed song was reported as failed and retried.
Android source `6fdb061a` accepts complete files in the cache, the normal
media directory, or an account-recorded custom path. Source `bce320af` also
makes the Retry control name both actions when it retries a failed search and
failed songs together; an invalid song remains counted as failed without an
inert Retry control.

The signed `bce320af` Android candidate was tested in a large channel on a
disposable account. Its chat menu and dialog showed the intended action and
defaults. The progress row reached **339 of 339 downloaded** with no failed
tracks, at most two active transfers, and remained attached after leaving and
reopening the chat. Pause and resume changed the row state correctly. The
app-managed external cache contained 134 recently modified MP3 files and no
recent temporary files; the standard Telegram Audio folder was empty. This
supports real cache completion, though a per-song channel-to-file inventory
was not collected in that run. An earlier signed APK from source `3559ef67`,
which includes a separate notification
privacy fix, installed and showed the chat-menu action with the account intact.

The desktop implementation is installed in the daily-use app and has passed
an app launch smoke test; its menu and cache behavior have not yet been
runtime verified.

On 2026-09-28 the disposable Android test app repeated the default action in
a test channel. The row reached 339 of 339 selected songs. An extension count
of its managed cache found 328 MP3, seven M4A, one FLAC, and three MP3 files with
additional filename suffixes, matching 339 audio-looking files in total. The
older observation of 134 *recently modified* MP3 files was not a complete
inventory. Equal totals did not prove that every selected document had a
matching file; a document-to-path comparison was needed. The old job had
disappeared after the app process ended, as its state is memory-only.

The follow-up audit found a storage mismatch: the prior Android job used
FileLoader's general cache mode, while normal chat music playback checks the
managed document media path. Individual messages could therefore still show a
download arrow even while the pinned-song row said all were downloaded. The
user-linked example had a complete file of the expected size in the general
cache on the disposable Android account. The Android fix downloads new songs
to the path chat playback checks and promotes valid legacy cache-only files to
that path. Cache-only files count as incomplete until promotion succeeds.

The signed Android build with this fix and the per-file progress list was
tested on the same disposable account on 2026-09-29. The default action
completed at **339 of 339**. A private inventory matched 339 distinct selected
message IDs to 339 distinct document keys in Telegram's normal Documents
directory; every file existed and had positive size. The previously affected
song lost its chat download arrow, and its row showed Downloaded. Scrolling
the file list remained responsive. For a controlled retry, one small completed
file was moved to a safe backup. Reopening the list reduced the count to
338 of 339, showed a clear missing-file reason and a per-file Retry action.
Retry restored a byte-identical normal-path file and the row returned to
339 of 339. The extra backup was then removed. No account data was cleared.

Android source `8818448c` replaces the fixed-height file dialog with a
full-screen list opened by tapping the progress bar. It searches song titles,
artists, and original filenames. Tapping a row jumps to that message in the
chat; the separate play/pause control uses Telegram's player, and failed rows
keep their Retry action. Playback started from the list follows chat message
order among the currently shown downloaded songs. The chat More menu also has
**Storage used by this chat**, which opens Telegram's per-chat cache sheet after
its scan or explains that no files were attributed to the chat. It combines
the old and new peers of a locally known migrated group.

The signed test build of `8818448c` preserved the disposable account and
completed the large-channel job at 339 of 339. Emulator checks covered search
by title and artist, a Persian filename, no-match and clear states, play/pause
and external media controls, and the storage sheet. The first build exposed a
row-tap bug, which was fixed before the second build. In that build, row taps
jumped to exact messages both unfiltered and after filtering; the filtered
case loaded older history while the chat was showing recent messages. Tapping
Play stayed in the list, and jumping during playback kept the song playing.
No fatal exception appeared in logcat. Per-row Retry was not repeated for this
UI change; the earlier controlled missing-file test covered the unchanged
transfer engine. Migrated-group and forum-topic jumps still lack live QA.

The final signed arm64 APK was rebuilt from clean Android commit `8818448c`,
verified with the release signing certificate, and installed over the test
account without clearing its data. It opened the Chats UI without a login
prompt. The emulator's pixel capture was black during this last smoke test,
so that check used Android's UI hierarchy; the interaction checks above were
visually observed on the signed test build of the same source patch. The final
APK was delivered privately on 2026-09-29.

The Android selection follows Telegram's `isMusic` classification. It includes
documents with non-voice audio attributes and MIME fallbacks for FLAC, OGG and
Opus. Generic audio documents without those attributes may be omitted even if
named MP3 or M4A. The pinned-message search pages until an empty response and
signals a scan failure if page IDs stop decreasing; album expansion requests
100 nearby messages. These boundaries should be checked if a user reports a
song absent from the selected count.

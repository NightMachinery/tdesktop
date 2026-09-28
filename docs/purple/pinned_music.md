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
was not collected. Retry remains untested because the run had no failures. A
final APK with a separate notification privacy fix is being built.

The desktop implementation is installed in the daily-use app and has passed
an app launch smoke test; its menu and cache behavior have not yet been
runtime verified.

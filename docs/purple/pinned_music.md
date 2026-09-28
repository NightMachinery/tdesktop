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
visible completion and failure counts. No APK with that change has been built
or tested yet, so the transfer fix is unverified. The desktop implementation is
installed in the daily-use app and has passed an app launch smoke test; its
menu and cache behavior have not yet been runtime verified.

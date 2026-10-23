# Saving content

Purple allows local Save As, downloads and media saving even when a chat or
message restricts saving content. This includes file and photo context menus,
selected-message downloads, the shared-media list and the media viewer. Stories
can be downloaded without a local no-forward or Premium check, and the viewer
does not show a locked save control for them. There is no new setting.
Protected chats also expose Export Chat History, and protected rich messages can
be saved as HTML. Existing special-chat, membership and server export checks apply.

TTL voice messages and round videos expose a guarded Save action while their
media is available. TTL photos and ordinary videos keep their existing service
placeholder, but gain Save As when the server supplies a full media payload.
Before a save is requested, Purple retains only media metadata. It clears that
save source when the existing timer expires or a view-once item is consumed.
Absent or empty payloads remain unavailable. These actions do not change
forwarding permissions or bypass a server refusal.

Save-only sources own their pending photo download subscription. Old menu and
file-dialog callbacks use a weak source guard, and recheck the source's expiry
before requesting or writing a file. Expiry, consumption, replacement and message
deletion therefore invalidate those callbacks. Voice and round-video actions use
a source token observing item removal and recheck identity and expiry after the file dialog.
Replacing the media invalidates that token even if an allocator reuses its address.
Contents-read updates from another client clear save-only photo/video sources
before the ordinary voice/mention eligibility filter. Full-payload editions retain
the unread or expired service text independently of their save-only media.
Unnamed voice saves retain the original MIME-aware default: `.mp3` for
`audio/mp3`, `.ogg` otherwise. Unnamed round videos use `.mp4`; existing
filenames are preserved. A download already explicitly started by the user
follows the existing document download behavior.

The Desktop app and its window helpers have no content-dependent screenshot or
screen-capture exclusion API in this checkout. Protected chats, stories and the
TTL media layer therefore need no screenshot-policy change here. Operating-system
permission to capture the screen is independent of this policy.

## Upstream hooks

The always-on policy and save-only sources live in `purple/purple_user_control`,
registered in `Telegram/cmake/purple.cmake`. Upstream changes are limited to
policy calls:

- `history/history_inner_widget.cpp` permits media saving, selection for batch
  downloads and TTL document menus. Its copy guards use the existing Purple copy
  policy so they cannot hide selected-file downloads.
- `history/view/media/history_view_save_document_action.cpp` permits TTL document
  Save actions in secondary lists and delegates TTL document saving to Purple.
- `menu/menu_item_download_files.cpp` admits protected messages into a local
  download batch without changing their forwarding flags.
- `info/media/info_media_provider.cpp` permits shared-media selection while
  preserving the frozen-session guard.
- `data/data_story.cpp` permits story downloads at both eligibility checks.
- `data/data_peer.cpp` lifts the protection-flag gate for chat history exports.
- `iv/iv_rich_message_html_export.cpp` lifts that gate for Save as HTML.
- `history/history_item.cpp` delegates TTL payload construction to Purple and
  clears save-only sources at expiry, contents consumption and consumed server
  editions. Its service-text condition recognizes TTL placeholders independently
  of the save-only media.
- `api/api_updates.cpp` expires save-only sources on contents-read dispatch before
  the ordinary unread-media/mention filter.
- `history/view/history_view_context_menu.cpp` and the legacy chat menu call
  Purple to add a save-only source's Save As action.

The media viewer and secondary-list photo/save guards already use the unrestricted
Purple copy policy described in [copy_markdown.md](copy_markdown.md). Message and
peer flags, server-side saved sounds, and expiry handlers remain upstream-owned.

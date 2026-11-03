# Media playback

Purple's changes to how voice and round (video) messages play on Desktop. The
design, with the reasoning and the Android side, is in
[media_design.md](media_design.md); this page describes what is built.

The code is in `Telegram/SourceFiles/purple/`:

- `purple_media_playback.{h,cpp}`: the app side of every feature below.
- `purple_media_rules.{h,cpp}`: the decisions behind the hooks, with no Qt and
  no app types, so the cases in `purple/tests/` run them on their own.
- `hooks/media_playback.h`: the facade that upstream files include.

## Double-click restarts from 0:00

A double-click on the voice or round message that is the current voice-type
track seeks it to 0:00 and plays it, even if it was paused.

- It works in the chat history (`HistoryInner`) and in the other message
  lists, such as replies, pinned and scheduled messages (`ListWidget`).
- It is declined, and upstream's double-click runs as before, for any button
  other than the left one, while messages are selected, for a handler that
  does not open a document, for a document that is not a voice or round
  message, for view-once media, for a message that is not the current track,
  and for a track that is not streaming yet (nothing to seek).
- The first click of the pair still does what a click does: a playing message
  pauses for under one double-click interval, then restarts. A stopped one
  starts, then jumps to 0:00 once it streams. Waiting for a possible second
  click would delay every single click, so it is not done.
- When the restart happens the double-click never reaches
  `mouseActionStart`, so the second release toggles nothing and the
  Reply or React quick action does not run.
- The floating round's own double-click is unchanged: it opens the message.

## Upstream hooks

Every hook is listed in [upstream_hooks.md](upstream_hooks.md), section
"Media playback". `purple/test_media_hooks.py` (gate `test_media_hooks`)
fails when a merge drops one of them.

- `history/history_inner_widget.cpp`, `HistoryInner::mouseDoubleClickEvent`,
  and `history/view/history_view_list_widget.cpp`,
  `ListWidget::mouseDoubleClickEvent`: one early return through
  `Purple::RestartOnDoubleClick`, after the read-metrics call and before
  `mouseActionStart`.

## Tests

- `purple/tests/media_restart_cases.cpp` (gate `cases-media-restart`): the
  restart decision for every reason to decline.
- `purple/test_media_hooks.py` (gate `test_media_hooks`): the hook pins.
- Needs a GUI check after a build: double-click a playing and a paused voice
  message and round, in a chat and in a replies thread.

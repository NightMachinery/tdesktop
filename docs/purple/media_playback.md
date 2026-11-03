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

## Video message size

Settings > Advanced > Purple > Media playback > Video message size chooses how
large a round message is drawn in chats: 100, 150, 200 or 250% of upstream's
240px, or Actual size. The default is 200% (480px).

- Actual size shows each round's own pixels one to one on this screen: its
  source side divided by the device pixel ratio. The choice is labelled with
  the percentage that gives for a 400px Desktop recording, so it reads
  "Actual size (83%)" on a 2x screen and "(167%)" on a 1x one. A round whose
  size is unknown is treated as 400px.
- The choice is device-local, in the generic preference store under
  `purple-video-message-scale` (a percent, or `actual`). A missing or
  unknown value reads as 200%. It is not in `settings.toml`, because a size
  belongs to one screen.
- Unlike upstream, a round is scaled up as well as down. In a chat column
  narrower than the size the round takes the width there is, as photos do.
- 100%, rounds hosted in an instant view and other media keep upstream's
  sizing exactly.
- Above the 430px message width the bubble limit is raised to the round's
  width with its reply or forward header, so 200% is not clipped. When the
  round carries an ephemeral plate Purple leaves upstream's limit, which is
  already computed from the larger round.
- Changing the size re-measures every round message in loaded chat
  histories at once. Rounds in other lists (replies, pinned, scheduled) take
  the new size when the list is next opened. Moving the window to a screen
  with another pixel ratio changes Actual size from the next layout.
- Larger sizes look softer: a 400px recording drawn at 480px on a 2x screen
  is a 2.4x upscale. The row's caption says so.

Known limit: `Gif::countOptimalSize` measures with `st::msgMaxWidth` as a
fixed bound, so Purple reads exactly that width as "no limit". A chat whose
room for the round is exactly 430px wide therefore shows the full size for
that one width and may overflow it by the difference.

## Upstream hooks

Every hook is listed in [upstream_hooks.md](upstream_hooks.md), section
"Media playback". `purple/test_media_hooks.py` (gate `test_media_hooks`)
fails when a merge drops one of them.

- `history/history_inner_widget.cpp`, `HistoryInner::mouseDoubleClickEvent`,
  and `history/view/history_view_list_widget.cpp`,
  `ListWidget::mouseDoubleClickEvent`: one early return through
  `Purple::RestartOnDoubleClick`, after the read-metrics call and before
  `mouseActionStart`.
- `history/view/media/history_view_gif.cpp`, `Gif::countThumbSize` and
  `Gif::bubbleWidthLimit`: one early return each, through
  `Purple::VideoMessageThumbSize` and `Purple::VideoMessageBubbleWidthLimit`.

## Tests

- `purple/tests/media_restart_cases.cpp` (gate `cases-media-restart`): the
  restart decision for every reason to decline.
- `purple/tests/media_size_cases.cpp` (gate `cases-media-size`): the stored
  preference, the sides for every choice including Actual size, the column
  clamp and the bubble limit.
- `purple/test_media_hooks.py` (gate `test_media_hooks`): the hook pins.
- Needs a GUI check after a build: double-click a playing and a paused voice
  message and round, in a chat and in a replies thread; each size, Actual
  size on a 1x and a 2x screen, a narrow window, and a round with a reply.

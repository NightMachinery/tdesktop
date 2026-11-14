# Media playback

Purple's changes to how voice and round (video) messages play on Desktop. The
design, with the reasoning and the Android side, is in
[media_design.md](media_design.md); this page describes what is built.

The code is in `Telegram/SourceFiles/purple/`:

- `purple_media_playback.{h,cpp}`: the app side of every feature below.
- `purple_media_restart.{h,cpp}`: the guarded history restart FIFO.
- `purple_media_rules.{h,cpp}`: the decisions behind the hooks, with no Qt and
  no app types, so the cases in `purple/tests/` run them on their own.
- `purple_voice_queue.{h,cpp}`: the voice queue tracking and walk, with the
  pure parts in `purple_voice_queue_rules.{h,cpp}`.
- `hooks/media_playback.h`: the facade that upstream files include.

## Double-click restarts from 0:00

A double-click on the voice or round message that is the current voice-type
track seeks it to 0:00 and plays it, even if it was paused.

- It works in the chat history (`HistoryInner`) and in the other message
  lists, such as replies, pinned and scheduled messages (`ListWidget`).
- It is declined, and upstream's double-click runs as before, for any button
  other than the left one, while messages are selected, for a handler that
  does not open a document, for a document that is not a voice or round
  message, for view-once media, and for a message that is not the current
  track.
- The message is the click handler's own when it has one. The seek areas
  (a round's whole circle, a voice message's waveform) are built without
  one, and upstream returns them only while the hovered message is the
  current track, so a handler with no message whose document is the current
  track's takes the current track's message.
- A track that is streaming is seeked to 0:00. One that is current but not
  streaming yet (its length is not known) is played again from its start,
  which restarts the stream; the position saved for it was already used up
  by the first click.
- The first click of the pair still does what a click does: a playing message
  pauses for under one double-click interval, then restarts. A stopped one
  starts, then starts again from 0:00. Waiting for a possible second
  click would delay every single click, so it is not done.
- History queues its restart on the same main queue as the first click's
  document action. The first click therefore finishes before restart even
  when native input arrives in a batch. The full voice identity is checked
  again at execution, and seekability is read then. Destruction of the
  player, originating list or original window cancels the restart. The float
  keeps its synchronous restart because its first click toggles directly.
- When the restart happens the double-click never reaches
  `mouseActionStart`, so the second release toggles nothing and the
  Reply or React quick action does not run.
- The floating round restarts the same way on a double-click (see "Floating
  round controls"); its old double-click action, opening the message, moved
  to a hover button.

## Private-chat voice queue

When a voice or round message ends, upstream plays the next one in the chat
(unless the experimental `disable-autoplay-next` option is on). With
`[playback] private_chat_queue_p` on, the default, a queue you start in a
one-to-one chat on the other person's message plays only their messages and
skips yours. Settings > Purple > Media playback > Keep the voice
queue to the other person switches it; the value lives in `settings.toml`
([config.md](config.md)), so it follows the file to other devices.

- The rule is purple-core's `AnchorForManualPlay` and `QueueKeeps`
  (`purple/purple_playback.h` in the submodule), so Android applies the same
  one. "Theirs" means incoming: a message they forwarded to you counts as
  theirs, one you forwarded as yours.
- One-to-one means a user chat other than Saved Messages, Replies and
  Verification Codes. Bots count. A manual play on your own message, in Saved
  Messages, in a group or in a channel changes nothing.
- Manual or automatic: `purple_voice_queue.cpp` listens to the player.
  `switchToNextEvents` announces the message an automatic advance is about to
  play; a voice-type `trackChanged` to any other message is manual (a click in
  a chat, the player bar's next or previous, the control socket) and sets a
  new anchor for that chat. The announcement expires on the next main-loop
  turn, so one no track change followed (upstream's play returned early) can
  never make a later manual play look automatic. The setting is read only
  then, so toggling it never changes a queue in the middle.
- The walk starts at the message upstream would play and skips your messages
  until one of theirs. It stops at the edge of the loaded playlist window
  (about 32 messages either side), so more than about 31 of your own voice
  messages in a row end the queue. When none is left the queue ends as it
  does today.
- Only automatic advances of the voice-type player in the anchored chat are
  confined. Songs, manual next and previous, and other chats follow upstream.

## Video message size

Settings > Purple > Media playback > Video message size chooses how
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
  histories at once, in every list that shows it (`requestItemResize`).
  Rounds only loaded in another list (replies, pinned, scheduled) take the
  new size when that list is next opened. Moving the window to a screen
  with another pixel ratio changes Actual size from the next layout.
- Larger sizes look softer: a 400px recording drawn at 480px on a 2x screen
  is a 2.4x upscale. The row's caption says so.

Known limit: `Gif::countOptimalSize` measures with `st::msgMaxWidth` as a
fixed bound, so Purple reads exactly that width as "no limit". A chat whose
room for the round is exactly 430px wide therefore shows the full size for
that one width and may overflow it by the difference.

## The floating round keeps its size

When a playing round scrolls out of view it continues in the floating player.
Upstream draws that at 128px; with Media playback > Keep the size when it
floats (on by default) it is drawn at the in-chat side from "Video message
size" instead, plus the float's shadow margins.

- It is capped at half the shorter side of the window, so it never covers
  most of a small window, and it is never smaller than upstream's float.
- Off restores upstream's 128px.
- The size is fixed when the float is created. Resizing the window
  afterwards does not resize it; the next round that floats gets the new cap.
- Docking to a corner and closing by dragging out work on the widget's own
  size, so a larger float follows the same rules and needs a longer drag to
  close.
- Device-local, like the size: `purple-video-message-float-keep`.

## Floating round controls

The floating round is still dragged as before, and dragging it mostly off
the window still closes it. Hovering over it shows a row of three buttons
across the middle of the circle, where it is widest, so the row fits the
stock 128px circle too:

- Open the message: what a double-click on the float did upstream. It goes
  through the same callback, so the chat opens the same way.
- Play or pause, with the glyph following the player.
- Close: stops the voice-type player, as dragging it off does.

A second row under them seeks back and forward by the seek step; see
[media_recording.md](media_recording.md).
Step seeking keeps the floating video in place and displays its previous
frame while the same message's decoder prepares the new position. A temporary
missing frame does not trigger the slide-out and slide-in animation. Stop,
close, switching messages, and hiding the float behind its chat still work
normally; a float that has not appeared yet still waits for its first frame.

Clicks land only on the buttons; a press anywhere else still drags, and a
single click still pauses or resumes. A double-click on the float now
restarts the round from 0:00, like a double-click on a round in a chat,
including one that is not streaming yet. The buttons hide while the float is
dragged and come back when the drag ends with the pointer still over it.

Following the queue: when the next voice or round message starts on its own,
upstream already leaves the chat where it is if you scrolled away.
`HistoryWidget::scrollToCurrentVoiceMessage` and
`ListWidget::scrollToCurrentVoiceMessage` scroll only when the message that
just finished is still on screen and you have not scrolled in the last
second, and the floating round shows the new message whenever it is off
screen. So Purple changes nothing there.

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
- `media/player/media_player_instance.cpp`, `Instance::moveInPlaylist`: one
  guarded early return through `Purple::ConfinedVoiceNext`, right after
  `useIndex` is computed. It plays the chosen message through upstream's own
  `jumpByItem`, so the chat still follows it the way upstream does. The
  tracking needs no upstream line: it starts from the existing
  `Purple::OnMediaPlayerStarted` call in `core/application.cpp`.
- `media/player/media_player_float.cpp`, `Float::Float`: one line after the
  stock size, `size = Purple::FloatPlayerSize(parent, document, size);`.
  The shadow, frame and progress arc all follow `size` already.
- The same file: `Purple::DecorateFloat(this, _doubleClickedCallback)` at the
  end of `Float::Float` (hover controls), and one early return through
  `Purple::RestartFloatOnDoubleClick` at the top of
  `Float::mouseDoubleClickEvent`.

## Tests

The restart and size fixtures link QtCore. The Debug precompiled header uses
its version-tag symbol even when the tested rules themselves use no Qt types.

- `purple/tests/media_restart_cases.cpp` (gate `cases-media-restart`): the
  restart decision for every reason to decline, seek against replay, and the
  handler-context resolution.
- `purple/tests/media_restart_fifo_cases.cpp` (gate
  `cases-media-restart-fifo`): the retained FIFO helper with real
  `ActivateClickHandler` and CRL dispatch, both initial playback states and
  queued or settled first clicks, waveform and single-click controls,
  changed identity, player/list/window destruction and late seekability.
  Audio state and the history widget are modeled; no decoder or native
  mouse-event delivery is exercised.
- `purple/tests/media_size_cases.cpp` (gate `cases-media-size`): the stored
  preference, the sides for every choice including Actual size, the column
  clamp, the bubble limit and the float size with its window cap.
- `purple/tests/voice_queue_cases.cpp` (gate `cases-voice-queue`): the walk
  over a fake playlist (skips yours, stops at the edge, declines when not
  confined), the manual or automatic classification with its expiry, and
  which chats count as one-to-one, with the real purple-core rule.
- `purple/test_media_hooks.py` (gate `test_media_hooks`): the hook pins.
- Needs a GUI check after a build: double-click a playing and a paused voice
  message and round, in a chat and in a replies thread; each size, Actual
  size on a 1x and a 2x screen, a narrow window, a round with a reply, and
  the float with the toggle on and off in a large and a small window, and
  the float's hover buttons, drag and double-click, and a private chat where
  their message, then yours, then theirs play in turn.

Player seek button lookup asks Qt for a named QWidget and checks its concrete
button type with C++ RTTI. AbstractButton has no Q_OBJECT of its own, so Qt's
typed findChild cannot safely use it as the queried metadata type.

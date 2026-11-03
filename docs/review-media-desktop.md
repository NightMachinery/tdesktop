# Review: media F1-F5, Desktop

Scope: `1c8c3d9..media` (six commits, `717f076` to `d068058`), read against
`docs/purple/media_design.md`, its coordinator answers, and the owner's
decisions since then: the float's double-click restarts (F2 wins), "go to
message" moves to a hover button, drag stays, and F5 gets "Actual size".

Verdict: **CHANGES**. One P1: double-click restart does nothing on a round in
a chat, or on the waveform of an incoming voice message. Everything else is
P2 or P3.

## How this was checked

- Read every hunk in upstream files and every new Purple file, then traced
  the upstream call paths they depend on: `Instance::moveInPlaylist`,
  `setCurrent`, `play`, `playStreamed`, `clearStreamed`, `emitUpdate`,
  `finishSeeking`, `seekStreamed`, `Gif::textState`,
  `Gif::clickHandlerPressedChanged`, `Gif::countOptimalSize` and
  `countCurrentSize`, `Message::resizeGetHeight`, `Document::textState`,
  `Document::createComponents` and `refreshParentId`, `FloatController`
  (`create`, `toggle`, `updatePosition`), and `HistoryWidget` and
  `ListWidget::scrollToCurrentVoiceMessage`.
- Ran `purple/test_media_hooks.py`: 18/18.
- Built the pure cases with `g++ -std=c++20 -Wall -Wextra`:
  `media_restart_cases` 9/9, `media_size_cases` 44/44. `voice_queue_cases`
  passed 18/18, but against a stub of purple-core's `purple_playback.h`
  written from the design. The purple-core repository is outside this
  session's access, so the real rule and the submodule bump were not read.
- I did not build the app here. Compile-level questions about the app-bound
  files (includes, `enumerateUsers` taking a `not_null<PeerData*>` lambda)
  are unverified.
- Out of scope: the Android F1 analysis (the "could not reproduce the
  close" note). It lives in the Android repository, which this session
  cannot read. Someone with access to that repository needs to check it.

## P1

### P1-1. Double-click restart never fires on rounds, or on an incoming voice message's waveform

`Purple::RestartOnDoubleClick` (`purple/purple_media_playback.cpp:396-425`)
takes the message from the active handler: `open->context()`. Both seek
handlers are built with no context, and nothing sets one later:

- Round: `Gif` builds `_seekl` as
  `std::make_shared<VoiceSeekClickHandler>(_data, [](FullMsgId) {})`
  (`history/view/media/history_view_gif.cpp:251-253`). The constructor's
  context defaults to `FullMsgId()` (`data/data_file_click_handler.h:69-73`).
  `Gif` does not override `refreshParentId`, so `_seekl` is never given an id.
- Voice: `Document::createComponents` builds `voice->seekl` the same way
  (`history_view_document.cpp:478-481`). Its id is set only in
  `Document::refreshParentId` (`:2027-2030`), which runs only from
  `Session::notifyItemIdChange` (`data/data_session.cpp:2156-2167`). That
  happens when one of your own sent messages gets its server id, never for
  an incoming message.

Trace, for a playing round in a chat:

1. While the round is current and not stopped, `Gif::isRoundSeekable()` is
   true (`gif.cpp:2500-2512`), so `Gif::textState` returns `_seekl` for the
   whole circle (`gif.cpp:1559-1561`). That covers the paused state too.
2. First click: on release, `Gif::clickHandlerPressedChanged` calls
   `playPauseCancelClicked(Voice)` and the round pauses.
3. Double-click: `HistoryInner::mouseDoubleClickEvent` calls
   `RestartOnDoubleClick(_seekl, LeftButton, false)`. Then:
   - `context` is `FullMsgId()`;
   - `item` is `owner().message(FullMsgId())`, which is null;
   - `currentTrack` is false, because `current.contextId() != FullMsgId()`;
   - `seekable` is false, because `RestartsCurrentVoice` starts with
     `context && ...`.
   So the hook returns false.
4. Upstream's `mouseActionStart` runs, and the second release toggles again.
   The round resumes where it was. It does not restart.

So in-chat F2 never works on a round, in either list, playing or paused. For
voice it works only on the play button (`_openl`, which `setDocumentLinks`
builds with the context) and on the waveform of your own sent messages. An
incoming voice message's waveform, the largest target, behaves as upstream.
The float restart (`RestartFloatOnDoubleClick`) is not affected, because it
uses the float's `_item`.

The cases can't catch this. `media_restart_cases.cpp` tests only the `&&` of
facts that are already computed, and the bug is in how those facts are
computed.

Fix, inside Purple and with no new upstream line: when the handler's context
is empty and its document is the current voice-type track, use the current
track's context. This is safe because both seek links are returned only
while the hovered message is the current track: `isRoundSeekable` and
`Document::textState` (`:1420-1428`) both compare `state.id` against
`_realParent->fullId()`. Put the resolution in a pure helper in
`purple_media_rules`, for example
`ResolveRestartContext(handlerContext, handlerDocMatchesCurrent, currentContext)`,
and add cases: an empty handler context that matches the current track,
one that doesn't, and a non-empty context.

## P2

### P2-1. A not-yet-streaming double-click falls back to two toggles, and the doc says otherwise

`docs/purple/media_playback.md` says "A stopped one starts, then jumps to
0:00 once it streams". In the code, `RestartsCurrentVoice` needs
`state.length > 0` and a state that isn't stopped. If the first click has
started the track but its length isn't known yet when the double-click
arrives, the hook declines. Upstream's second release then hits `_openl`
again, and `playPause` pauses the track. So double-clicking a round you have
not played yet can start it and pause it straight away. This is plausible,
not confirmed: it depends on how quickly the streaming info arrives, which
can't be timed without a GUI.

Suggested fix: when the handler is the current track's (`currentTrack`, not
view-once, voice or round) but it isn't seekable yet, still return true, and
make sure it plays (`play(Voice)` if paused). Either way, fix the doc
sentence.

### P2-2. The purple-core bump brings in unrelated, unreviewed core work

`3f37f0f` moves `Telegram/ThirdParty/purple_core` to `6d0bd81`. Its message
says this brings "the magic sync chat work beneath them on the core's media
branch". `Telegram/cmake/purple.cmake` now compiles
`purple/purple_sync_chat_magic.{h,cpp}` from core. That is sync-chat code
riding on a media branch. It may change behavior that has nothing to do with
media, and this review cannot read it because the purple-core repository is
out of scope here. If the core `media` branch is later rebased, the pointer
breaks.

Before merge:
- move the pointer to a purple-core commit on its main line;
- land the sync-chat magic work through its own reviewed bump, or say in the
  commit which of its behavior is live in this build.

## P3

- **P3-1. Untested app glue.** No case covers `ChatOf`: Saved Messages,
  Replies and Verification Codes must map to Self or Other, and bots to
  OneToOne. The same goes for the context resolution in P1-1. Both are small
  pure functions once the peer flags are passed in.
- **P3-2. Stale automatic announcement.** `PlayTracker` keeps `_automatic` if
  `_switchToNext` fires but no voice `trackChanged` follows. That happens
  when `play()` returns early because `sharedDocument` is null. A later
  manual play of that same message is then taken as automatic and keeps the
  old anchor. To fix it, clear `_automatic` on the next `trackChanged` of any
  type, or when `_tracksFinished` fires.
- **P3-3. Lists outside the chat history don't relayout on a size change.**
  `RelayoutVideoMessages` walks only `history->blocks`, so replies,
  scheduled, pinned and sublist `ListWidget`s keep the old size until they
  are reopened. This is documented. Firing `requestItemResize(item)` instead
  of `requestViewResize(view)` would reach every view of the item through
  `enumerateItemViews`.
- **P3-4. The 430px exact-width overflow.** `VideoMessageThumbSide` treats
  `widthMax == st::msgMaxWidth` as "no limit". This is documented. A real
  `countCurrentSize` width of exactly 430px gives a 480px round that
  overflows by 50px.
- **P3-5. The float doesn't follow a size change.** An existing float keeps
  its size when "Video message size" or "Keep the size when it floats"
  changes, or when the window shrinks. This is documented.
- **P3-6. Hover buttons after a drag.** The buttons hide during a drag and
  stay hidden after it ends, even with the pointer still over the float,
  until the next Enter. Show them again on the release that ends the drag.
- **P3-7. Button placement at stock size.** With F4 off (152px float, 128px
  circle), the outer two 32px buttons reach about 70px from the circle's
  centre, past its 64px radius, so they stick out of the circle. Shrink the
  row, or move it up, when the inner side is small.
- **P3-8. `Lifetime` isn't destroyed at shutdown.** The static `Lifetime` in
  `purple_voice_queue.cpp` lives until static destruction, after
  `Media::Player::finish`. rpl's weak disposal makes this harmless today.
  Destroying it from a shutdown hook would match the other services.

## Verified as correct

- **F3 automatic versus manual.** The order holds:
  1. `jumpByItem` fires `_switchToNext` before `play()`;
  2. `playStreamed` → `clearStreamed` → `emitUpdate` sees the old id, so
     `setCurrent` is a no-op;
  3. the following `emitUpdate` runs `setCurrent(new)` and fires
     `trackChanged`, so `PlayTracker::manual` returns false.
  Manual next or previous (`autonext == false`), songs, and other chats all
  return nullopt, and upstream runs unchanged. Voice has no shuffle, reverse
  or repeat (`order`, `repeat` at `instance.cpp:1377-1394`), so the forward
  walk matches upstream's direction. With the setting off,
  `AnchorForManualPlay` doesn't confine, and the hook is a no-op.
- **"No jump to an auto-played next message when scrolled away."**
  `HistoryWidget::scrollToCurrentVoiceMessage` (`history_widget.cpp:1810-1840`)
  scrolls only when the finished message isn't completely hidden and you
  haven't scrolled in the last second. The doc's claim is right, and no hook
  is needed.
- **F2 hooks when they decline.** These are early returns before
  `mouseActionStart`, so upstream's double-click (word selection, quick
  reply and react) is untouched whenever Purple declines.
- **F5 hooks.** At 100%, in instant view, for non-rounds and with an
  ephemeral plate, both `Gif` hooks return empty or 0, and upstream's sizing
  runs exactly as before. `st::maxVideoMessageSize` has no other upstream
  user, so nothing else assumes 240px. A relayout goes through
  `setPendingResize`, so `initDimensions` re-runs (`element.cpp:1648-1652`).
- **Prefs.** A missing key reads as 200% and keep-size on. Bool false is
  stored as an empty value and read back as false. Both are saved through
  `saveSettingsDelayed`.
- **F4.** The float's shadow, frame, mask and arc all derive from the
  widget size. Positioning and `outRatio` use `size()`. No upstream code
  reads `st::mediaPlayerFloatSize` outside the constructor.
- **Hook shape.** Every upstream hunk is one facade include, one guarded
  call, or one early return. The upstream_hooks.md line counts match the
  diff: +7, +7, +14, +9, +6. The third float hook, `RestartFloatOnDoubleClick`,
  goes beyond the design but follows the owner's decision and is documented.
- **Privacy.** No personal paths, emails or tokens in the diff, and all six
  commits are authored by `noreply`.

## Re-check 1

Scope: commit `8842259` on `origin/media`, checked against the findings
above. I traced each fix and looked only for regressions the fix introduces.

The pure cases pass, with `voice_queue_cases` still built against the same
purple-core stub:
- `test_media_hooks.py`: 18/18;
- `media_restart_cases`: 15/15;
- `media_size_cases`: 44/44;
- `voice_queue_cases`: 25/25.

I still did not build the app. The new `crl::on_main` calls, including the
guarded one on `not_null<Float*>`, are not compile-checked.

Verdict: **CHANGES**, only because P2-2 is untouched. The code fixes are
sound.

### Closed

- **P1-1: closed.** Take a playing or paused round in a chat:
  1. `textState` returns `_seekl`, whose context is empty.
  2. `ResolveRestartContext(FullMsgId(), true, current.contextId())` returns
     the current track's id. `FullMsgId` has `operator!`, so the template
     instantiates.
  3. `owner().message(...)` finds the item, so `currentTrack` is true, and
     `seekable` is true while the track streams.
  4. The result is `Restart::Seek`: `finishSeeking(Voice, 0.)`, then
     `play(Voice)` if paused. The hook returns true, so the second release
     toggles nothing.

  An incoming voice message's waveform (`voice->seekl`, empty context)
  takes the same path. The fallback only applies when the handler's
  document is the current track's. Both seek links are returned only while
  the hovered message is the current track (`Gif::isRoundSeekable`,
  `Document::textState`), so it can't pick another message. The new context
  cases cover all four combinations.
- **P2-1: closed.** For a track that is current but not seekable yet,
  `RestartOnDoubleClick` returns `Replay`, which calls `play(current)`.
  `playStreamed` restarts the stream. `clearStreamed(data, false)` saves no
  position, because the audio is unchanged, and the first click already used
  up the saved position. So it starts at 0:00. `setCurrent` sees the same
  id, so no `trackChanged` fires and the voice-queue anchor is untouched.
  The doc now matches this.
- **P3-1: closed.** `PlaybackChatFor` is pure, with cases for Self, Replies,
  Verification Codes, user or bot, and Other.
- **P3-2: closed.** `expire` is queued with `crl::on_main` after
  `automaticNext`. The automatic `trackChanged` fires synchronously inside
  the same `jumpByItem` → `play` → `playStreamed` → `emitUpdate` stack (see
  "Verified as correct" above), so `manual()` consumes the announcement
  before the expiry runs. An announcement that nothing consumed is cleared
  one loop turn later. `expire(key)` only clears a matching key.
- **P3-3: partly closed, as documented.** `requestItemResize(view->data())`
  now reaches every view of each item (`enumerateItemViews`), including
  `ListWidget` views of items that are in loaded history blocks. Items held
  only by another list (scheduled messages, thread-only loads) still wait
  until that list is reopened, which the doc now says.
- **P3-6: closed.** On a `MouseButtonRelease` on the float, a queued
  `show(underMouse())` runs after `Float::mouseReleaseEvent` → `finishDrag`
  has cleared `_drag`. The call is guarded by `floating`. A drag-out close
  detaches the item first, so `show()` keeps the buttons hidden.
- **P3-7: closed.** The row is now centred vertically. At the stock size the
  outer centres are 38px from the circle's centre, and 38 + 16 = 54 < 64, so
  the buttons stay inside the circle.
- **P3-4 and P3-5:** documented, accepted. **P3-8:** skipped, accepted
  (harmless).

### Still open

- **P2-2: not addressed.** The commit doesn't touch
  `Telegram/ThirdParty/purple_core`. The pointer still sits on core's
  `media` branch with the magic sync-chat work, and `purple.cmake` still
  compiles `purple_sync_chat_magic.cpp`. Either fix it before merge, or the
  coordinator or owner records that they accept it as is. Once they do,
  this re-check becomes APPROVE.

### Regression introduced by the fix (P3)

- **P3-9. The centred row covers the float's middle.** On hover, the
  108x32 strip across the centre is now buttons. A press there doesn't drag
  or single-click-pause; it hits one of the buttons, which does the same
  thing as a click. A double-click there doesn't restart, because it lands
  on an `AbstractButton`: on the pause button that is two toggles, and on
  the open or close button it is a double action. It also covers the centre
  of the video while hovered.
  - Restart still works everywhere else on the circle, so this is P3.
  - Suggested fix: keep the row low but inset it to fit the circle, for
    example its top at `inner.center().y() + inner.height() / 4 - side / 2`.
    Or forward a double-click on any button to
    `Purple::RestartFloatOnDoubleClick`.

# Review: Desktop F9, seek back and forward

Scope: `38031b1..recording` at `903eb4a` (three commits: the purple-core bump,
the seek buttons, shortcuts and system media controls, and the settings row).
Read against `AGENTS.md` and `docs/purple/media-recording-design.md` (F9 and
its parts of Settings, Upstream hooks and Tests, with the owner's answers).
Read-only review: nothing on `recording` was changed. This VM cannot build the
app, so every trace below is from reading the code.

Verdict: CHANGES. P1=1 P2=2 P3=6.

## What was checked and is fine

- **purple-core gitlink** is exactly `d7f378534f0f8829e3249dbc51f1e650d12f66d3`
  ("Read the recording table and the seek step"). It is on purple-core's
  `recording` branch; that branch has since moved three commits ahead
  (`58f305a`, `5256098`, `fb1ceba`), which this branch does not need.
- **Clamping.** `SeekRules::SeekTarget` goes to 0:00 before the start, to one
  second before the end past it, never moves backwards from inside the last
  second, and ignores unknown lengths. `recording_cases.cpp` compiles on its
  own with `g++ -std=c++20` and passes 24/24.
- **Paused vs playing.** `SeekBy` uses `Instance::updateSeeking` (which calls
  `seekStreamed` with `keepPaused`) for a paused track and `finishSeeking` for
  a playing one, so a paused voice, round or song stays paused. Rapid repeated
  clicks accumulate: after `Player::play` the stage is `Initializing`, the
  legacy state is Playing, and the position reads back as
  `_options.position`, the target just set.
- **Loading and streaming.** Before the stream knows its length,
  `prepareLegacyState` falls back to the document's duration; with neither,
  forward does nothing and back is dropped by `seekStreamed`'s own duration
  guard. No crash path found.
- **Voice, round, music.** The bar seeks the bar's own `_type`; the float
  always seeks `Voice`, the only type the float shows; the shortcuts and the
  system controls seek `getActiveType()`. Bar, float and in-chat views all
  redraw from the same `emitUpdate`, so they stay in sync.
- **Float layout.** At the stock 128px float the second row's button circles
  stay inside the round (outer edge about 58px from the centre against a 64px
  radius). The row hides with the first row while dragging.
- **Settings.** `[playback] seek_step` is written through `WriteSettings` and
  `SetTableString` (pinned in `wait_routes.json`), read back by purple-core
  with the 5/10/15/30 rule, and every label follows `SettingsChanges()` live.
  The chooser's initial index is always valid because `StepFrom` maps any
  value to a listed step.
- **Hook shape.** Each upstream hunk is an include, one call, one early
  return or one data line. `upstream_hooks.md` has an entry per file.
  `purple/test_media_hooks.py` passes 34/34 and the `test_media_hooks` and
  `test_wait_routes` host gates pass; the switch from `'{'` to `'{\n'` in
  `body()` is needed, because `Widget::Widget`'s initializer list holds
  lambdas.
- **Privacy.** No names, paths, accounts or other personal data in the diff.

## P1

### 1. On macOS the default seek shortcuts do nothing while a chat is open

`alt+shift+left` and `alt+shift+right` are Option+Shift+Left/Right on macOS,
which is the platform's "select previous/next word" key
(`QKeySequence::SelectPreviousWord` / `SelectNextWord` on macOS).

Trace: open any chat. The message field gets focus. Press Option+Shift+Left
while a voice message or song plays. Qt sends `ShortcutOverride` to the
focused widget before it tries the shortcut map. The field is a `QTextEdit`,
whose text control accepts `ShortcutOverride` for the common text-editing
keys (`QInputControl::isCommonTextEditShortcut`, which covers the word
selection keys). The key goes to the field as a word selection. The
`QAction` for `MediaSeekBackward` never fires, so `Purple::StartMediaSeek`'s
handler is never asked.

The message field is focused nearly whenever a chat is open, so on the
owner's Mac the requested shortcuts work only when focus happens to be
elsewhere. `docs/purple/media_recording.md` already lists this as a "GUI
check", but it is a defect, not something to observe: the defaults do not
work in the main case. The Linux and Windows word keys are Ctrl+Shift+Arrow,
so there the defaults do fire, also inside text fields.

Fix: choose a different default on macOS (`#ifdef Q_OS_MAC` in
`fillDefaults` would be one more hook line; or one cross-platform pair that
is not a text-editing key on any platform). Whichever it is, check it against
`QKeySequence::keyBindings` for each platform and against upstream's
defaults, and pin it in `test_media_hooks.py`. The owner should pick the
keys.

## P2

### 2. The seek keys are taken app-wide, even when nothing is playing

The new commands are not in `MediaCommands` (`core/shortcuts.cpp`), so their
`QAction`s are enabled from startup with `Qt::ApplicationShortcut`, instead
of being switched on by `Shortcuts::ToggleMediaShortcuts` while the player is
active, as play, pause, previous and next are. When `SeekActive` returns
false, `Launch` returns false, but Qt's shortcut map has already consumed the
key press for an enabled exact match. Two existing behaviours lose the key:

- macOS keyboard text selection in messages: `HistoryInner::keyPressEvent`
  extends a text selection by word on Shift+Option+Left/Right
  (`KeyboardTextSelection::extend`, `byWord` on `Qt::AltModifier`).
  `HistoryInner` does not accept `ShortcutOverride`, so the shortcut eats the
  key, always, with or without media.
- The media viewer is another window of the same application, so the
  shortcut fires there too. With a video open, Alt+Shift+Left/Right used to
  reach `OverlayWidget::handleKeyPress`, whose timestamp jump tests only
  `Qt::AltModifier`. Now it seeks the background audio, or with none, does
  nothing.

Fix: add both commands to `MediaCommands` (one more upstream line each), so
they exist only while a track is loaded, and pick keys that avoid finding 1.

### 3. After a song ends, back and forward silently do nothing

Song ends with no next track and repeat off: the bar stays up and the state
is `StoppedAtEnd`. `SeekBy` rejects any state for which
`IsStoppedOrStopping` is true and returns false, so "−10" on the bar, "−10"
on the float and the shortcut all do nothing. The typical use, "replay the
last ten seconds", fails at exactly the moment it is wanted. The system
media controls disagree: there `HandleSystemMediaCommand` returns false and
upstream's own branch runs, which calls `finishSeeking` to 15 s before the
end and plays. So the OS skip works, with a step other than the setting,
while Purple's own controls do not.

Fix: for `StoppedAtEnd`, and for a stopped track that is still `current`,
seek back from the length, starting playback like upstream's branch, or at
least stay paused at the target. Forward from the end stays a no-op. Return
true for skip commands in that state too, so the OS path uses the step.

## P3

### 4. The docs misstate upstream's system media controls

`media_recording.md` says "Upstream skipped by 15 seconds for video only";
the design's Current behaviour says the same. Upstream's
`SystemMediaControlsManager` constructor also handles `SkipForward` and
`SkipBackward` for audio (`kSkipMs`, 15 s, through `finishSeeking`, which
resumes a paused track). The hook therefore does change upstream behaviour
for audio: the step instead of 15 s, a paused track stays paused, and a
one-second end guard. That is what the owner asked for, but the feature doc
and the `upstream_hooks.md` entry should say that this is what changes.

### 5. A paused seek leaves the OS media widget at the old position

`SystemMediaControlsManager` updates the platform position only on
`Seeking::Finish` or a playback status change. A paused seek goes through
`updateSeeking`, which fires neither, and the status stays Paused through
`distinct_until_changed`. After a paused "−10" from the bar, the shortcut or
the OS skip button, the system widget's scrubber is wrong until playback
resumes. Firing a Finish-equivalent update from Purple is not possible
without a hook; one option is to call `finishSeeking` and then `pause` for
the paused case, if that does not blip audio. Narrow; note it at least.

### 6. The clamping rule exists twice, and the two copies disagree

`purple_seek_rules.cpp` reimplements a rule that purple-core states once as
`Purple::SeekTarget` in `purple/purple_recording.h`, from `fb1ceba` on its
`recording` branch. The two differ: with an unknown length, core's forward
"only adds the step", Desktop's does nothing. The pinned `d7f3785` already
has a comment in `purple_settings.h` that names "SeekTarget() in
purple_recording.h", a file that does not exist at that commit. When the
gitlink next moves, switch Desktop to core's function, drop
`purple_seek_rules`, and settle the unknown-length rule in one place. Until
then, say in `media_recording.md` that the local copy is temporary.

### 7. Possible signed overflow in `SeekBy`

`state.length * 1000LL / state.frequency` and the same for the position.
`frequency` is always `kMsFrequency` (1000) for streamed tracks, so the
multiplication does nothing, but if `computeTotalDuration` returns
`kDurationUnavailable` (int64 max; possible when the stream has no duration
and no packets have been counted yet) it overflows, which is undefined
behaviour. Use the values directly, or reject `length >= kDurationMax`.

### 8. Holding a seek key seeks once

Neither command is in `AutoRepeatCommands`, so `setAutoRepeat(false)`
applies. Holding the key to scrub, as upstream allows for previous and next,
does a single step. If repeat is wanted it is one more data line in the
same set; otherwise say in the doc that it is deliberate.

### 9. `getTimeRight` walks the widget tree on every mouse move

`PlayerSeekWidth` runs `findChild` (recursive) twice, and
`Widget::updateOverLabelsState(QPoint)` calls `getTimeRight` on each mouse
move over the bar. Cheap at today's tree size, but keeping the two button
pointers in a property or a small per-widget state set at decoration avoids
it.

## Tests

- The design asks `recording_cases.cpp` to cover "the seek step, and the
  shortcut command names". The step and clamping are covered; the names are
  pinned line by line by `test_media_hooks.py` instead, which is acceptable
  and documented.
- Missing: nothing exercises `SeekBy`'s state guard (the stopped case in
  finding 3) or the paused versus playing branch. These need the player, so
  at least list them under the GUI checks in `media_recording.md`, with the
  macOS shortcut check (finding 1) and the media viewer case (finding 2).
- Whatever keys fix finding 1, extend `SHORTCUT_LINES` to pin them, including
  any `MediaCommands` entries added for finding 2.

## Re-check 1

Scope: `903eb4a..2e58de3` on `recording` (one commit, "Fix the seek keys and
seeking back after a track ends"). Read-only; `recording` was not changed.
Checks run here: `recording_cases.cpp` with `g++ -std=c++20 -Wall -Wextra`
passes 41/41 with no warnings; `purple/test_media_hooks.py` passes 40/40; the
`test_media_hooks` and `test_wait_routes` host gates pass. The app still
cannot be built on this VM.

Verdict: CHANGES. P1=0 P2=1 (finding 3 is not fixed). No new P1 or P2.

### Findings from the first pass

- **1 (P1), macOS keys: fixed.** `fillDefaults` now calls
  `set(Purple::SeekShortcutKeys(±1), ...)`, which gives `ctrl+alt+left/right`
  on macOS (Command+Option+Arrow, since Qt maps `ctrl` to Command and this
  app does not set `AA_MacDontSwapCtrlAndMeta`) and keeps `alt+shift+left/right`
  elsewhere. Command+Option+Arrow is not in Qt's macOS text-editing key table
  (word moves are Option+Arrow, line moves Command+Arrow, selection adds
  Shift), so `QInputControl::isCommonTextEditShortcut` does not claim it and
  a focused message field no longer swallows the key. Upstream binds no
  Left/Right with Command+Option (its `ctrl+alt` defaults are Home and End).
  The `QString` argument binds to the `set(const QString &, Command, bool)`
  overload (an exact match beats the conversion to `QKeySequence`), so the
  keys are parsed as `PortableText` like every other default. On Windows and
  Linux, word selection is Ctrl+Shift+Arrow, so Alt+Shift+Arrow in a text
  field reaches the seek, as intended.
- **2 (P2), keys taken app-wide: fixed.** Both commands are now in
  `MediaCommands`, so their actions start disabled and are switched by
  `ToggleMediaShortcuts`. That means on from `MainWidget` showing the player
  bar, and off in `Instance::stopAndClose`. With no bar, macOS word
  selection in the message list and the media viewer's Alt+Arrow timestamp
  jump get their keys back. One narrow leftover, documented by the author:
  on Windows and Linux, while the bar is shown, Alt+Shift+Arrow in the media
  viewer still seeks the background audio instead of jumping a timestamp.
  Plain Alt+Arrow still works there. Not counted.
- **3 (P2), back after a song ends: not fixed.** `PlanSeek` now treats a
  stopped current track as sitting at its end and plans `SeekPlaying` to
  `length − step`, and a case tests that. That state is never reached in
  the app. Trace: a song plays to its end. `handleStreamingUpdate` gets
  `Finished`, calls `emitUpdate`, and then, because `player().finished()`
  is true, calls `clearStreamed(data)`, which sets `data->streamed` to
  null. `handleStreamingError` does the same after a failure, and
  `Instance::stop` does it for a user stop. From then on
  `Instance::getState(type)` returns a default `TrackState`, whose `id` is
  empty. So `SeekFacts.current` is false and `PlanSeek` returns `Decline`.
  The bar still shows the song, and "−10", the float and the shortcut do
  nothing. Even if `SeekPlaying` were planned, `finishSeeking` would reach
  `seekStreamed`, which returns at once with no `streamed`.
  `media_recording.md` now promises that "−10" right after a song ends
  replays its last ten seconds; it does not. The case "back after the end
  replays the last step" only proves the plan for a state the player never
  reports.

  Fix, still without a hook: when the type's `current()` is set and there
  is no streamed state, `SeekBy` with direction −1 starts
  `player->play(current)`. On the first `updatedNotifier` state for that id
  with a known length, it then calls `finishSeeking(type, (length − step) /
  length)`. Forward stays a no-op. If that is not wanted, drop the stopped
  branch and the doc promise instead.

  Correction to my first pass: I wrote that the system media controls
  "disagree" here because upstream's skip branch replays from 15 s before
  the end. That was wrong for the same reason: after `clearStreamed`,
  upstream's branch sees `state.length == 0` and also does nothing. The
  remaining defect is only that Purple's controls do nothing.
- **4 (P3), docs on upstream's audio skip: fixed.** `media_recording.md` and
  the `upstream_hooks.md` entry now say that the hook changes upstream's
  audio skip (step instead of 15 s, paused stays paused, end guard).
- **6 (P3), duplicate clamping rule: addressed as requested.** It is kept as
  a documented temporary copy, with the unknown-length difference stated
  and the switch to core's `Purple::SeekTarget` planned for the next gitlink
  move. Acceptable while the gitlink must stay at `d7f3785`.
- **7 (P3), overflow: fixed.** `SeekRules::ToMs` divides before it
  multiplies and rejects anything over `kLongestMs` (equal to upstream's
  `kDurationMax`), so `kDurationUnavailable` reads as unknown. The bounds
  hold: the whole-seconds part is at most 2147483 before `* 1000`, and the
  remainder term is below `frequency * 1000`.
- **8 (P3), auto-repeat: fixed.** Both commands are in `AutoRepeatCommands`.
  Repeats while playing each restart the stream at the accumulated target
  (`prepareLegacyState` reads back `_options.position` while
  re-initialising), so a held key scrubs steadily, with audio restarting on
  each step. That is acceptable for a scrub.
- 5 and 9 are listed under "Known follow-ups" in `media_recording.md`, as
  allowed.

### New defects from the fixes

- None at P1 or P2. Shortcut handling was checked on macOS (above), in text
  fields on every platform, and with the player bar hidden.
- Hook shape: `core/shortcuts.cpp` gains the facade include after its own
  header (pinned), two data lines in each of two sets and the two
  `fillDefaults` calls; each hunk reads as a hook. The facade now pulls
  `data/data_audio_msg_id.h` and the system media controls header into
  `core/shortcuts.cpp`. That costs only compile time, so it is not counted.
- `HandleSystemMediaCommand` now returns true for `Consume` (at 0:00, or
  inside the last second) and false for `Decline`, so upstream's skip runs
  only for tracks Purple cannot seek. That is consistent with the docs.

### Still to do

1. Finding 3: make "−10" after a song ends work as described above, or
   remove the stopped branch, its test and the doc promise.

## Re-check 2

Scope: `2e58de3..8f792fa` on `recording` (one commit, "Replay the last seek
step after a track ends"). Read-only; `recording` was not changed. Checks run
here: `recording_cases.cpp` with `g++ -std=c++20 -Wall -Wextra` passes 46/46
with no warnings; `purple/test_media_hooks.py` passes 40/40. The change
touches no upstream file.

Verdict: APPROVE. P1=0 P2=0. Finding 3 is fixed; two new P3 notes below.

### Finding 3: fixed, through upstream's own resume path

Trace: a song plays to its end. `handleStreamingUpdate` gets `Finished` and
calls `clearStreamed`, so `getState(Song)` is a default `TrackState` with an
empty id, while `current(Song)` still names the song (the bar still shows
it). `SeekBy` now passes `ended = !state.id && document` and the
document's own `duration()` (milliseconds; `-1` when unknown).
`PlanSeek` takes the `!current` branch:
`SeekTarget(length, length, -1, step)` gives `length − step`, or 0 for a
track shorter than the step. The result is `Restart` at that position.
Forward gives `Consume`, and an unknown or over-long length gives
`Decline`.

`RestartAt` stores the target with
`Storage::Account::setMediaLastPlaybackPosition` and calls
`Instance::play(current)`. That call is synchronous all the way into the
upstream code that reads the position:

1. `play` gets `sharedDocument`.
2. `playStreamed` calls `clearStreamed(data, false)`, a no-op because there
   is no stream, so it does not overwrite the stored position.
3. `streamingOptions(audioId)`, with the default position `-1`, reads
   `mediaLastPlaybackPosition` into `result.position` and resets it to 0.

This is the same path upstream uses to resume a long song. If `play`
returns early (no shared document, or not an audio, voice or video
message), the stored value is still the target, and `RestartAt` clears it.
The value 0 is handled too: `setMediaLastPlaybackPosition` erases the entry
for `time <= 0`, and the stream then starts at 0:00. `setCurrent` returns
early because `data->current` already equals the id, so passing no
playlist context does not reset the playlist. That matches upstream's own
`play(type)` for the bar's play button. A voice restart goes through
`pauseSongForVoice`, as any voice play does.

The system media controls take the same route: `Restart` is not `Decline`,
so upstream's skip branch (which would see `length == 0`) is not reached.
The stopped branch with a live stream now declines, and its old test was
replaced; the "after the end" cases test the state the player really
reports. `media_recording.md` now describes the real mechanism. The new
`// WHY:` comment is two lines, within `AGENTS.md`'s allowance.

### New notes (P3, not blocking)

- **A failed track is retried.** `handleStreamingError` also calls
  `clearStreamed`, so a track that failed to open, or is not streamable,
  also reads as "ended". "−10" or the OS skip-back then calls `play` again.
  For `NotStreamable` and `OpenFailed`, that repeats upstream's
  `DocumentSaveClickHandler::SaveAndTrack` on the next failure. It is
  narrow, and the same as pressing play on the bar; a check of
  `document->loading()` or a remembered failure would avoid it.
- **One saved resume point can be evicted.** When the per-account list of
  saved positions is full (`kMaxSavedPlaybackPositions`, 256),
  `setMediaLastPlaybackPosition` drops the oldest entry before adding the
  restart position, which `streamingOptions` then removes again. Each
  restart can therefore delete the oldest saved resume point of some other
  long track. It needs 256 saved positions and is easy to miss; it only
  matters if the owner keeps that many.

### Status

All P1 and P2 findings from the first review are fixed. The open items are
P3 follow-ups: the paused-seek scrubber and the `findChild` walk, both
listed in `media_recording.md`; the media viewer's Alt+Shift+Arrow while the
bar is shown on Windows and Linux; and the two notes above.

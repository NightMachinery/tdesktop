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

# Media recording

Purple's media recording work on Desktop. The design, with the reasoning, the
Android side and the features still to build (the recording dock, F6 to F8,
F10 and the extras), is in
[media-recording-design.md](media-recording-design.md); this page describes
what is built. So far that is F9.

The code is in `Telegram/SourceFiles/purple/`:

- `purple_media_seek.{h,cpp}`: the app side of F9 (buttons, shortcuts,
  system media controls and the settings row).
- `purple_seek_rules.{h,cpp}`: the step and the clamping, with no Qt and no
  app types, so `purple/tests/recording_cases.cpp` runs them on their own.
- `hooks/media_recording.h`: the facade that upstream files include.

## F9. Seek back and forward

The voice, round and music player jumps back or forward by the seek step:
5, 10, 15 or 30 seconds, 10 by default.

- The player bar under the top bar has two buttons, "−10" and "+10" (the
  labels follow the step), in its right-hand controls just before the
  volume button. They seek the track the bar shows: a song, or a voice or
  round message.
- The floating round shows the same two buttons in a second row under its
  open, play and close buttons while the pointer is over it. They seek the
  round.
- Two shortcut commands, `media_seek_backward` and `media_seek_forward`.
  The default keys are `alt+shift+left` and `alt+shift+right` on Windows
  and Linux, where the text fields' word selection is Ctrl+Shift+Arrow.
  On macOS Option+Shift+Arrow is word selection (a focused message field
  would take the key), so the defaults there are `ctrl+alt+left` and
  `ctrl+alt+right`, that is Command+Option+Left and Right.
  `SeekRules::SeekShortcutKeys` decides; the owner may still pick other
  keys. They can be rebound in the custom shortcuts file and on the
  keyboard shortcuts settings page (Seek back, Seek forward).
- The shortcuts are media commands, like play and next: they are switched
  on only while the player bar is shown (`ToggleMediaShortcuts`), so with
  nothing loaded the keys reach text fields, the message list and the
  media viewer as before. While a track is loaded they still win over the
  media viewer's own Alt+Arrow timestamp jump.
- Holding a seek key repeats it, as holding previous or next does.
- They seek the active track: a voice or round message while one plays,
  otherwise the song.
- The system media controls' skip backward and skip forward commands seek
  the active audio track by the step. This changes upstream for audio:
  upstream skips audio by 15 seconds through `finishSeeking` (which
  resumes a paused track) and clamps to the very end. With Purple the
  jump is the step, a paused track stays paused, and forward stops one
  second before the end. While a video is the controls' delegate upstream's
  15 second video skip runs unchanged. Whether the platform shows skip
  buttons for audio depends on its integration.

Rules (`SeekRules::PlanSeek` and `SeekRules::SeekTarget`):

- A seek past the end goes to one second before it, so the queue does not
  advance by surprise. Forward inside that last second does nothing rather
  than jump back.
- A seek before the start goes to 0:00.
- A track that ended (or was stopped) but is still the current one
  seeks back from its end and plays, so "−10" right after a song ends
  replays its last ten seconds. Forward there does nothing.
- A track whose length is not known yet, or that is not the player's
  current track, is not seeked. The system controls then fall back to
  upstream's handling.
- A paused track stays paused after the seek; a playing one keeps playing.
- Lengths are converted to milliseconds without overflow; a length above
  about 24 days (upstream's `kDurationMax`), including the "unavailable"
  marker, counts as unknown.
- The clamping rule is a temporary local copy. purple-core states the same
  rule as `Purple::SeekTarget` in `purple/purple_recording.h` on its
  `recording` branch, after the commit this checkout pins; the two differ
  only for an unknown length (core's forward adds the step, this one does
  nothing, and Desktop never seeks an unknown length anyway). When the
  gitlink next moves past it, Desktop switches to core's function and
  `purple_seek_rules` keeps only the Desktop parts.

The step is `[playback] seek_step` in `settings.toml`, `"5s"`, `"10s"`,
`"15s"` or `"30s"`, read by purple-core into `Playback::seekStepSeconds`
(any other value warns and keeps 10). Settings > Advanced > Purple > Media
playback > Seek step writes it. Changing it relabels the buttons at once.

## Upstream hooks

Every hook is listed in [upstream_hooks.md](upstream_hooks.md), section
"Media recording (dock, background, PiP, seek, crash safety)".
`purple/test_media_hooks.py` (gate `test_media_hooks`) fails when a merge
drops one of them.

- `media/player/media_player_widget.cpp`: the facade include;
  `Purple::DecoratePlayerSeek` in `Widget::Widget`, just before its
  `setType` so the first layout already counts the buttons;
  `Purple::PlacePlayerSeek` in `Widget::updateControlsGeometry`, after the
  volume toggle, which places the buttons and returns their width; and
  `Purple::PlayerSeekWidth` in `Widget::getTimeRight`, which adds that
  width so the time label and the controls' fade make room.
- `media/system_media_controls_manager.cpp`: the facade include and one
  early return through `Purple::HandleSystemMediaCommand` at the top of the
  `commandRequests` handler, taken only when no video delegate is set.
- `core/shortcuts.h`: `MediaSeekBackward` and `MediaSeekForward` in the
  `Command` enum. `core/shortcuts.cpp`: their names in `CommandByName` and
  their default keys in `Manager::fillDefaults`. Purple handles both in its
  own `Shortcuts::Requests()` subscription, started from
  `Purple::OnMediaPlayerStarted`.
- `settings/sections/settings_shortcuts.cpp`: two rows in the shortcuts
  list, so the commands can be rebound in the UI.
- The floating round's buttons need no new hook: they come from the
  existing `Purple::DecorateFloat` call.

## Tests

- `purple/tests/recording_cases.cpp` (gate `cases-recording`): the step
  choices and their spelling, the clamping at 0, at one second before the
  end, inside the last second, for unknown lengths and for every step,
  the overflow-free conversion, the seek plan (not current, unknown
  length, playing, paused, after the end, at 0:00) and the default keys
  on each platform.
- `purple/test_media_hooks.py` (gate `test_media_hooks`): the hook pins,
  plus the shortcut enum entries, names, default keys and settings rows,
  pinned line by line.
- purple-core `tests/test_config.cpp`: reading `seek_step`.
- Needs a GUI check after a build: the buttons in the player bar for a
  song, a voice message and a round, in a narrow and a wide window; the
  float's second row on the stock 128px float and a larger one; both
  shortcuts with the message field focused and with a text selection in
  the message list, on macOS (Command+Option+Arrow) and elsewhere; that
  the keys do nothing special with no track loaded; holding a key; the
  media viewer's Alt+Arrow with and without background audio; "−10" right
  after a song ends and on a paused track; the media keys or system skip
  buttons where the platform shows them; and the step changing the labels
  live.

## Known follow-ups

- A paused seek does not move the system media widget's scrubber until
  playback resumes: `SystemMediaControlsManager` updates the platform
  position only on `Seeking::Finish` or a status change, and the paused
  path uses `updateSeeking`, which fires neither.
- `Widget::getTimeRight` asks `Purple::PlayerSeekWidth`, which walks the
  bar's children with `findChild`, on every mouse move over the bar.
  Cheap at today's tree size; keeping the two buttons in per-widget state
  set at decoration would avoid it.

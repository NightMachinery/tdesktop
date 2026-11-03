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
- Two shortcut commands, `media_seek_backward` and `media_seek_forward`,
  bound by default to `alt+shift+left` and `alt+shift+right`. They can be
  rebound in the custom shortcuts file and on the keyboard shortcuts
  settings page (Seek back, Seek forward). They seek the active track:
  a voice or round message while one plays, otherwise the song.
- The system media controls' skip backward and skip forward commands seek
  the active audio track by the step. Upstream skipped by 15 seconds for
  video only, and still does while a video is the controls' delegate.
  Whether the platform shows skip buttons for audio depends on its
  integration.

Rules (`SeekRules::SeekTarget`):

- A seek past the end goes to one second before it, so the queue does not
  advance by surprise. Forward inside that last second does nothing rather
  than jump back.
- A seek before the start goes to 0:00.
- A track that is stopped, or whose length is not known yet, is not
  seeked. A paused track stays paused after the seek; a playing one keeps
  playing.

The step is `[playback] seek_step` in `settings.toml`, `"5s"`, `"10s"`,
`"15s"` or `"30s"`, read by purple-core into `Playback::seekStepSeconds`
(any other value warns and keeps 10). Changing it relabels the buttons at
once.

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
  choices and their spelling, and the clamping at 0, at one second before
  the end, inside the last second, for unknown lengths and for every step.
- `purple/test_media_hooks.py` (gate `test_media_hooks`): the hook pins,
  plus the shortcut enum entries, names, default keys and settings rows,
  pinned line by line.
- purple-core `tests/test_config.cpp`: reading `seek_step`.
- Needs a GUI check after a build: the buttons in the player bar for a
  song, a voice message and a round, in a narrow and a wide window; the
  float's second row on the stock 128px float and a larger one; both
  shortcuts, including with the message field focused (on macOS,
  Option+Shift+Left also selects a word there); the media keys or system
  skip buttons where the platform shows them; and the step changing the
  labels live.

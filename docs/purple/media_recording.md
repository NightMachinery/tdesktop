# Media recording

Purple's media recording work on Desktop. The design, with the reasoning, the
Android side and the features still to build (the recording dock, F6 to F8
and the extras), is in
[media-recording-design.md](media-recording-design.md); this page describes
what is built: F9 and F10.

The code is in `Telegram/SourceFiles/purple/`:

- `purple_media_seek.{h,cpp}`: the app side of F9 (buttons, shortcuts,
  system media controls and the settings row).
- `purple_seek_rules.{h,cpp}`: the step and the clamping, with no Qt and no
  app types, so `purple/tests/recording_cases.cpp` runs them on their own.
- `hooks/media_recording.h`: the facade that upstream files include.
- F10, all in Purple's own files:
  - `purple_recording_crypt.{h,cpp}`: the encrypted file format (below).
  - `purple_recording_mirror.{h,cpp}`: the voice mirror, one file per
    capture object, written at the offsets FFmpeg writes.
  - `purple_recording_registry.{h,cpp}`: the manifest (`recordings.json`),
    its states, and the decisions on stop, on start and on a sent message.
  - `purple_recording_recovery.{h,cpp}`: voice recovery over purple-core's
    `FinaliseOgg`.
  - `purple_recording_store.{h,cpp}`: the app side: the key, the manifest
    on disk, the two voice hooks, recovery at start, the sweep and sent
    detection.
  - `purple_recording_bin_box.{h,cpp}`: the Recording bin box.
  - `purple_recording_settings.{h,cpp}`: the "Media recording" settings group.

  These files, apart from the store, the box and the settings, use no Qt
  and no app types, so `purple/tests/recording_cases.cpp` runs them alone.

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
- After a track ends the player drops its stream (`clearStreamed`), so
  there is nothing to seek. While that track is still the player's
  current one (the bar still shows it), back plays it again from its
  length minus the step, or from 0:00 if it is shorter: Purple stores that
  position with `setMediaLastPlaybackPosition` and calls
  `Instance::play`, whose `streamingOptions` starts from the stored
  position, the same path that resumes a long song. So "−10" right after a
  song ends replays its last ten seconds. If the play does not start, the
  stored position is cleared again. Forward there does nothing. The length
  is the document's own duration; an unknown one does nothing.
- A stream that is stopping is not seeked.
- A track whose length is not known yet, or that is not the player's
  current track, is not seeked. The system controls then fall back to
  upstream's handling.
- A paused track stays paused after the seek; a playing one keeps playing.
- Lengths are converted to milliseconds without overflow; a length above
  about 24 days (upstream's `kDurationMax`), including the "unavailable"
  marker, counts as unknown.
- The clamping rule is still a local copy. purple-core states the same
  rule as `Purple::SeekTarget` in `purple/purple_recording.h`, which the
  pinned gitlink now includes; the two differ only for an unknown length
  (core's forward adds the step, this one does nothing, and Desktop never
  seeks an unknown length anyway). Switching Desktop to core's function,
  so that `purple_seek_rules` keeps only the Desktop parts, is a pending
  follow-up.

The step is `[playback] seek_step` in `settings.toml`, `"5s"`, `"10s"`,
`"15s"` or `"30s"`, read by purple-core into `Playback::seekStepSeconds`
(any other value warns and keeps 10). Settings > Advanced > Purple > Media
playback > Seek step writes it. Changing it relabels the buttons at once.

## F10. Crash-safe recordings and the Recording bin

### Where things are kept

Everything lives in `tdata/purple/recordings/`: the manifest
`recordings.json` and one file per recording, named by a random 16-hex id
(`<id>.ogg.prc` for voice). Nothing in the names says which chat a
recording belongs to.

Each manifest entry holds the id, the account index, the chat's peer id,
the forum topic's root id, a reply id (always 0 on Desktop, see below), the
kind, the start time, the duration, when it entered the bin, the view-once
flag, whether it is partial, whether it was stopped from the listen
(preview) state, its file names and its state: `recording`, `sending`,
`sent`, `unsent`, `discarded`, `recovered` or `unrecoverable`. The manifest is
rewritten whole on every change, through a temporary file and a rename.

### Encryption at rest

Owner answer 4 = A. Every file there, the manifest included, is
AES-256-CTR. The key is derived from the account's local key (the key
Telegram Desktop encrypts its own cache with, the same for every account)
with the archive's helpers: `DesktopArchiveCrypto::Derive` (HKDF-SHA256,
salt `purple-recording-v1`, info `recording-files`), `Random` for the IVs
and `Hmac` for the key check. A file starts with a 32-byte header: the magic
`PRC1`, version 1, two reserved bytes, a random 16-byte IV and the first 8
bytes of `HMAC(key, "check")`. The counter for byte `n` of the content is the
IV plus `n / 16`, so the mirror writes at any offset without rewriting what
is around it. A file whose check does not match (the local key changed, for
instance after every account logged out) is reported as written under
another key and never decrypted into garbage; a manifest in that state is
replaced by an empty one. CTR has no authentication, as the owner chose; a
tampered file decrypts to wrong bytes, which recovery then treats like any
damaged recording.

The key exists only once an account's session is up, which is after the
passcode. Until then nothing is mirrored.

### Voice

- `Purple::MirrorRecordingWrite` in `Instance::Inner::Private::WriteData`
  (`media/audio/media_audio_capture.cpp`) gets every block FFmpeg writes,
  with its offset, before upstream copies it into memory. The first write at
  offset 0 opens a new encrypted file and a manifest entry in state
  `recording`; later writes go to their offsets, so seeks back behave as they
  do in memory and the file holds the same bytes as `Private::data`. Writes
  are flushed to the operating system each time, so a crash of the app loses
  at most the Ogg page being built (about a second).
- The chat is taken from the active window's current chat a moment after
  the first write, on the main thread. Desktop recordings cannot leave their
  chat yet (F8 is not built), so that is the chat the recording belongs to.
  Upstream's voice bar does not know the reply it would send with, so the
  reply id is 0.
- `Purple::NoteRecordingStop` at the top of `VoiceRecordBar::stopRecording`
  says Send or Cancel (Listen, the pause into preview, is ignored). Send moves
  the entry to `sending`. Cancel moves it to the bin as "Discarded" while
  `keep_discarded_p` is on (owner answer 1 = A), and deletes it once the file
  is closed while it is off.
- The file is closed when `Media::Capture::Instance` reports that it stopped,
  which is after the trailer was written, or at the next recording's first
  write if that report never came. A recording still in `recording` when it is
  closed was stopped without a decision and is treated as a Cancel.
- A sent voice or round message is noticed through `itemIdChanged`: the first
  waiting entry of the same account, chat and kind is the one that was sent,
  and its file and entry are deleted. Desktop does not keep unsent uploads
  across restarts, so until then the entry stays.
- Sending from the preview goes through upstream's Cancel (the bar sends,
  then hides with `StopType::Cancel`). That entry is marked as stopped from
  the preview, and a message of the same chat and kind sent within two
  minutes settles it as well. A recording discarded from the preview with no
  send stays "Discarded".
- If a write fails (a full disk), mirroring stops quietly for that recording
  with one log line, and the recording goes on as upstream does. The item is
  then marked partial.

### At start

Once the key is there, every entry still in `recording` or `sending` belongs
to a crash. On a background thread its file is decrypted and handed to
purple-core's `FinaliseOgg`, which cuts it after the last good page and sets
the end-of-stream flag; the fixed bytes are encrypted again. A `recording`
entry becomes "Recovered after a crash", a `sending` one "Not sent", and one
without any audio page "Could not be recovered" (its bytes are kept as they
were until auto-clear, and Save still exports them). Files in the directory
that no entry names are deleted.

### The Recording bin

Settings > Advanced > Purple > Media recording > Recording bin. The row
shows the number of items when there are any. The box lists them newest
first, each with the chat ("Unknown chat" when the account or chat is gone,
"Hidden chat" when the running Work Mode preset hides it), the kind, the
duration, when it was recorded, the account when there are several, and the
mark: "Recovered after a crash", "Partly recovered", "Not sent",
"Discarded" or "Could not be recovered". Per item:

- Play plays it in the box (a round in a small circle) through
  `Editor::SegmentPlayer`, from memory; nothing decrypted touches the disk.
- Send sends it to its own chat and topic, with the recording's own account,
  through `ApiWrap::sendVoiceMessage`, the route the voice bar uses. A voice
  recording gets its waveform and duration from `Media::TrimAudioToRange`.
  View-once is kept. The item waits in `sending` until the message is sent.
- Send to another chat opens upstream's chooser for the recording's account
  (`send_elsewhere_p`).
- Save writes the decrypted file where the user picks, the download folder by
  default. This is the only way a plain copy leaves the bin.
- Delete deletes it. Clear all asks first.

Items are cleared by `bin_keep` (12 h, 24 h, 48 h by default, 7 d or never),
counted from when they entered the bin, at start and every hour, by
purple-core's `BinItemExpired`. `keep_discarded_p` and `bin_keep` are rows in
the same group.

Desktop never had recordings on disk before this, so there is no orphan scan.

### Privacy

The app log gets counts and states only: how many items were recovered,
cleared or removed as strays, and that a mirror write failed. Never a file's
contents, a chat's name or id, a path or a recording's id.

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
- `media/audio/media_audio_capture.cpp`: the facade include and
  `Purple::MirrorRecordingWrite(l, l->dataPos, buf, buf_size)` in
  `Instance::Inner::Private::WriteData`, before the copy.
- `history/view/controls/history_view_voice_record_bar.cpp`: the facade
  include and `Purple::NoteRecordingStop(...)` at the top of
  `VoiceRecordBar::stopRecording`, with the stop type, whether the bar is in
  the preview, and the view-once flag. The stop type travels as its ordinal,
  so the test pins the order of `StopType`.
- The floating round's buttons need no new hook: they come from the
  existing `Purple::DecorateFloat` call.

## Tests

- `purple/tests/recording_cases.cpp` also runs F10's Qt-free parts against
  OpenSSL: the CTR keystream at any offset and across a counter carry, the
  encrypted file (no plain bytes on disk, another key refused, a future
  version, a foreign file, the atomic whole-file write and a fresh IV each
  time), the voice mirror (opening only at offset 0, seeks and rewrites, the
  close, the next recording after a close request, a refused open), the
  manifest (round trip, empty, future, torn, and an entry naming a path
  outside the directory), the stop decisions, the marks, sent matching
  (earliest first, by account, chat and kind, the preview window) and the
  sweep, and voice recovery of a torn Ogg stream through purple-core's
  `FinaliseOgg` (duration, a finished stream, junk kept as it was, another
  key).

- `purple/tests/recording_cases.cpp` (gate `cases-recording`): the step
  choices and their spelling, the clamping at 0, at one second before the
  end, inside the last second, for unknown lengths and for every step,
  the overflow-free conversion, the seek plan (not current, unknown
  length, playing, paused, stopping, after the end with no stream left,
  at 0:00) and the default keys
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

- F10 needs a GUI check after a build: a voice recording killed mid-way
  (the bin shows it recovered and it plays and sends), Send, Cancel with
  `keep_discarded_p` on and off, a send from the preview, the bin's Play,
  Send, Send to another chat, Save, Delete and Clear all, and the hourly
  sweep with a short `bin_keep`.
- The reply id is not captured on Desktop; F8's hook in
  `VoiceRecordBar::init` is the place that can give it.

- A paused seek does not move the system media widget's scrubber until
  playback resumes: `SystemMediaControlsManager` updates the platform
  position only on `Seeking::Finish` or a status change, and the paused
  path uses `updateSeeking`, which fires neither.
- `Widget::getTimeRight` asks `Purple::PlayerSeekWidth`, which walks the
  bar's children with `findChild`, on every mouse move over the bar.
  Cheap at today's tree size; keeping the two buttons in per-widget state
  set at decoration would avoid it.

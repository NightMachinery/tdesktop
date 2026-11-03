# Review: Desktop F10, crash-safe recordings and the Recording bin

Branch `recording`, head `1f4213134c`, range `8f792fae5..1f4213134c`. Read-only review; nothing was built or
run (this VM cannot build the app). Every P1 below has a trace through the code at that head, including the
upstream code the hooks sit in.

Read: `AGENTS.md`, `docs/purple/media-recording-design.md` (F10, the F10 parts of Settings, Upstream hooks and
Tests, and the owner's answers at its top), `docs/purple/media_recording.md`, `docs/purple/upstream_hooks.md`,
purple-core `docs/recording-formats.md` at the pinned gitlink, and all of the new Purple sources and tests.

Verdict: **CHANGES**. P1 = 4, P2 = 5, P3 = 6.

## What checks out

- The gitlink is exactly `a6631f8de3e813096b70dc63d01268fce009e4fd` (submodule populated and checked out).
- Hook shape. `media_audio_capture.cpp` +2, `round_video_recorder.cpp` +3, `history_view_voice_record_bar.cpp`
  +2: one include right after the file's own header and one call per hooked function. No upstream line is moved
  or reformatted. `upstream_hooks.md` has an entry per file, and `purple/test_media_hooks.py` pins each call and
  the `StopType` order. Hooking `writeFrame` instead of `copyPackets` is right, and the doc says why.
- None of the hooks changes upstream's sent bytes. The voice hook only reads the block. The round hook clones the
  packet. `OnRoundEncodingStarted` gets null after a failed header and returns.
- Journal choices are exactly Off, 1, 2 and 5 s, read when a round starts. Off still writes the config header
  (`EncodeRoundJournalStart` into `<id>.pjr.prc`, owner answer 2). `bin_keep` offers 12 h, 24 h, 48 h (the
  default), 7 d and never, and the pinned purple-core parses `"7d"`. `keep_discarded_p` is a toggle row.
- Encryption: AES-256-CTR with a fresh random 16-byte IV per file, including every `WriteWhole` and every
  recovery rewrite (through `.new` and a rename). The key comes from the domain local key, which every account
  shares, through HKDF, and an 8-byte HMAC check refuses a wrong key instead of decrypting garbage. Nothing is
  mirrored before a session exists, so a passcode-locked start writes nothing until unlock. Play decrypts into
  memory. The only plaintext that leaves the directory is Save, which the user asks for.
- purple-core's Ogg walk stops at a page of another serial or out of sequence, so a reused file never splices two
  recordings.
- The log lines are counts and states only. No contents, chat names, peer ids, paths or recording ids.

## P1

### P1-1. Sending from the preview leaves the sent recording in the bin as "Discarded"

The hook passes `isListenState()` to tell "sent from the preview" from a real discard, but by the time
`stopRecording` runs, upstream has already cleared `_listen`. So the flag is false on every path except the
bar's destructor.

Trace (voice; a round is the same with `.video = true`):

1. Record, lock, pause into the preview: `stopRecording(Listen)`. `NoteRecordingStop` ignores Listen, and the
   entry stays `recording`.
2. Press Send: `VoiceRecordBar::requestToSendWithOptions` fires `_sendVoiceRequests`, and
   `HistoryWidget::sendVoice` sends, then calls `_voiceRecordBar->clearListenState()`
   (`history/history_widget.cpp:5844`), which calls `hideAnimated()`.
3. When the animation ends, `hideAnimated` runs `hideFast(); stopRecording(StopType::Cancel);`.
   `hideFast()` calls `hide()`. `RpWidget::setVisible` fires `shownValue(false)` synchronously
   (`lib_ui/ui/rp_widget.h:291`, `rp_widget.cpp:330`). The bar's `shownValue` handler
   (`history_view_voice_record_bar.cpp:2239-2244`) calls `finish()`, which sets `_listen = nullptr`
   (`:2841`).
4. `stopRecording(Cancel)` then calls `Purple::NoteRecordingStop(..., isListenState() /* false */, ...)`.
   `DecideStop(Cancel)` puts the entry in state `discarded` with `fromListen = false`.
5. `itemIdChanged` for the sent voice reaches `MatchSent`, which only settles a `discarded` entry when
   `fromListen` is set ("a real discard is never settled by a send"). Nothing matches.

Result with `keep_discarded_p` on (the default): every voice or round sent after listening to it stays in the
bin as "Discarded" for 48 h, and Send there sends it a second time. With `keep_discarded_p` off, the file goes
into `deleteAfterClose` and is deleted as soon as the capture stops, while the upload is still running. A crash
during that upload then leaves nothing, where a direct send would leave "Not sent".

`stop(true)` has the same order (`hide()`, then `stopRecording`), so the flag is false there too. The only
caller that still sees `_listen` is `~VoiceRecordBar` (`:2141-2145`), which is a real discard; see P2-2.

`media_recording.md` ("That entry is marked as stopped from the preview") and the registry test ("a send from
the listen state settles its discarded entry") describe behaviour that the app never reaches. The test feeds
`fromListen = true` by hand.

Fix: track the preview inside Purple. `NoteRecordingStop(Listen)` already reaches Purple with the ids still
active, so it can mark them `fromListen`. A later Cancel then reads the flag from the entry, not from the hook
argument, and a resume clears it (the voice mirror sees writes continue; a round opens a new part). Then drop
the `listen` parameter from the hook. A store-level case is needed (P2-5).

### P1-2. A paused and resumed round leaves "Not sent" pieces after it was sent

Each encoder run of a round is its own manifest entry. Send marks all of them `sending`, but a sent message
settles only one.

Trace:

1. Record a round, pause into the preview (`RoundVideoRecorder::pause` calls `finish`), then tap the lock to
   resume. `RoundVideoRecorder::resume` calls `Private::restart`, which calls `initEncoding`, which calls
   `OnRoundEncodingStarted(this)`. `CloseActive` moves part A into `closedParts`, and part B is a new entry.
2. Press Send (`stop(true)`, then `stopRecording(Send)`). `NoteRecordingStop` gets A and B from
   `RequestRoundCopiesClose()` and sets both to `sending`.
3. Upstream sends one message (A and B joined by `appendToPrevious`). `HandleSent` calls `MatchSent`, which
   returns the earliest `started` (A). A is removed.
4. B stays `sending`. `FinishClosed` skips recovery for a round that is not in the bin. On the next start,
   `NeedsRecovery(sending)` recovers B with `wasSending = true`, so the bin shows the tail of an already sent
   round as **"Not sent"**, with a Send button.

After a crash during a resumed round, the bin shows two unrelated pieces where the user made one recording. The
design's Terms define a recording as one capture "including any pauses". `media_recording.md` documents the
two items but not the stuck `sending` part.

Fix: give the parts one group id. Settle, discard and expire per group, and list a group as one bin item
(remux the parts in order, as `appendToPrevious` does, when sending or saving).

### P1-3. A manifest that does not read cleanly deletes every recording

In `Load`, any manifest that is not read as `Ok` leaves `store.manifest` empty or partial. Then
`RemoveStrays(store)` deletes every `.prc` file that the in-memory manifest does not name, and the next
`SaveLocked` overwrites the old manifest. The recordings that need recovering are deleted before recovery runs.

Triggers:

- **Unknown version.** `Decode` returns `UnknownVersion` with no entries (after a downgrade, or once v2 exists).
  The design's edge cases say "an unknown version is listed as unrecoverable and kept raw". The code deletes
  everything instead.
- **Damage.** The manifest is rewritten on every state change, and `WriteWhole` renames without an fsync. After
  a power loss (the laptop dies mid-recording, which is the crash F10 exists for), the file can come back empty
  or zero-filled. That reads as `NotOurs`, and every file goes. One flipped bit is enough to fail the JSON parse,
  because CTR has no MAC.
- **More than 4096 entries.** The parser fails the whole array at entry 4097 (`kMaxEntries`). With
  `bin_keep = "off"`, the bin grows without limit, and P1-1 adds an item on every preview send.
- **A second data profile.** `Directory()` is `cWorkingDir() + "tdata/purple/recordings/"`, which does not
  depend on the data name. A profile started with `-key` has another local key, so `WrongKey`, and each profile
  wipes the other's bin on start.
- **A transient open failure.** `File::Open` reports `Missing` when the stream will not open (a sharing
  violation on Windows), which reads as "no manifest".

Fix: delete strays only when the manifest read was `Ok`, or when it was `Missing` and the file really does not
exist. Otherwise move the manifest aside (`recordings.json.bad`), keep every `.prc` file, and list the ones it
cannot place as "Unknown chat" or "Could not be recovered" items, which is what the design asks for. Flush the
temporary file to disk before the rename. Let an over-long array keep its first N entries instead of failing
the whole parse.

### P1-4. "Send" for a recording made in a forum topic posts to General

`OriginalThread` falls back to the whole history when `forumTopicFor(item.topic)` returns null, and
`Forum::topicFor` returns null for any topic that is not loaded yet (`data/data_forum.cpp:627-633`). Topics load
lazily when the forum is opened, so this happens right after a restart, which is when the bin is used.

Trace: record a voice in topic T of forum F, crash, restart, open Settings, Recording bin, press Send. Then
`OriginalThread` returns `history` (F), and `Api::SendAction(history)` has `topicRootId = 0`. The message goes
to F's General topic, in front of a different audience. The same happens when T was deleted.

Fix: when `item.topic != 0`, never fall back to the history. Use `forum->enforceTopicFor(item.topic)` (it
requests the topic) and send once it resolves, or hide Send until it does. Refuse when the topic was deleted.

## P2

### P2-1. A recording too short to send leaves junk in the bin

Upstream drops a voice shorter than one second: `Inner::stop` resets `d->dataPos = 0` and clears `d->data`,
then calls `av_write_trailer` (`media_audio_capture.cpp:505-512`, `:552-554`). The trailer's flush reaches
`WriteData` at offset 0. The mirror reads "closing and offset 0" as the next recording:

- it finishes the real entry through `_restarted`, and that entry stays `sending` (nothing is sent; the bar's
  `stop(false)` returns early because the bar is already hidden);
- it opens a new entry holding only the trailer bytes. `closeAll` then treats that entry as a Cancel, so it
  becomes a "Discarded" item at once.

After the next start, the stuck entry also shows as "Not sent" or "Could not be recovered". A round under
`kMinDuration` has the same stuck `sending` entry. Fix: treat a `sending` entry whose capture ended without a
send as discarded and delete it (it is under a second), and do not open an entry from inside a closing
recording's trailer. For example, open a new file only after `closeAll` has cleared the owner, or only when
the capture is running.

### P2-2. Sends are settled by account, peer and kind only

`HandleSent` settles the earliest waiting entry for any outgoing voice or video message to the same peer. That
includes a forward: `ApiWrap::forwardMessages` creates local items that get `setRealId`. Two consequences:

- Forwarding a voice note into a chat while your own recording there is still uploading settles that recording.
  Its file is deleted, and a crash before the real upload finishes loses it.
- The one path where `fromListen` is true is a real discard: `~VoiceRecordBar` during the preview, for example
  closing a topic or replies section that owns its own `ComposeControls`. A voice sent to the same peer within
  2 minutes then matches the older discarded entry first (earliest `started`). The discarded recording
  disappears from the bin, and the new one stays `sending`, so it shows as "Not sent" after a restart.

Fix: for voice the mirror is byte-identical, so also require the sent document's size to equal the mirror's
plaintext size. For a send from the preview, after a trim, use the P1-1 flag plus the window. Ignore forwarded
items (`item->Get<HistoryMessageForwarded>()`).

### P2-3. Logout leaves the account's recordings, and the next account inherits them

Items are keyed by `Main::Domain` account index, and `Domain::add` reuses the lowest free index
(`main/main_domain.cpp:354-357`). Logging out of the only account keeps index 0 for the next login. Nothing in
F10 reacts to logout, so the previous user's recordings stay decryptable in the bin (the key is shared by every
account) for up to 48 h, or forever with `bin_keep = "off"`. The new user can then Play, Save and Send them. "Send" resolves
the old peer id in the new session and sends from the wrong account whenever that peer is loaded there. Fix:
on a session ending with logout (not on quit), delete or orphan that index's entries, and store a stable
account identity (the user id) next to the index.

### P2-4. "Unknown chat" with no Send button until the chat happens to be loaded

`HistoryOf` uses `historyLoaded`, which only finds histories that already exist in memory. Right after a restart,
or for a chat beyond the loaded dialogs, the item shows "Unknown chat" and has no "Send to the original chat".
The box rebuilds only on bin changes, so it never picks the chat up later. Fix: resolve through
`session->data().peerLoaded(peer)`, falling back to `history(peer)` once the peer is known, and rebuild on the
session's dialogs-loaded and peer-updated events.

### P2-5. The store's stop and settle flow has no test

`recording_cases.cpp` and `recording_round_cases.cpp` cover the pure pieces well: CTR, the mirror, the
manifest, `MatchSent`, voice recovery, the fragmented copy, repair. But nothing drives `NoteRecordingStop`,
`FinishClosed` and `HandleSent` the way upstream calls them, and nothing pins the hook arguments at call time.
P1-1, P1-2 and P2-1 all live there. Add a case that replays the call sequences: preview then send, preview then
discard, pause then resume then send, a sub-second recording, and a forward. The case can run over a fake mirror
and manifest, as the existing cases do.

## P3

- **P3-1. Synchronous I/O on the capture threads.** Every 4 KB voice block is encrypted, written and flushed
  inside `WriteData` on the capture thread. The first block also writes the manifest under the store mutex,
  which the main thread holds while it writes the manifest. The round copy does the same per packet on the
  recorder thread, under a mutex that the main thread holds for a whole trailer write. A stall over OpenAL's
  200 ms capture buffer would drop audio from the sent recording. Rare, because these are page-cache writes
  with no fsync, but a small queue to a writer thread would remove the risk.
- **P3-2. Double Send from the bin.** The item becomes `sending` only after the async remux, so two quick clicks
  send it twice. Mark it `sending` (or disable the button) before the async work starts.
- **P3-3. The bin can serve a round copy that is not finished.** For about 3 s after a stop, plus the remux,
  Play and Save read the fragmented copy (Send is protected since `1f4213134c`). Play may fail, and Save writes a
  fragmented MP4.
- **P3-4. View-once.** `viewOnce` is recorded only for a direct Send, which has no use after P1-1 is fixed.
  "Send to another chat" keeps `ttlSeconds` even for a group, where the server refuses it, and the item then
  stays `sending`. Drop the TTL outside private chats.
- **P3-5. The doc on key changes.** `media_recording.md` says the local key changes "after every account logged
  out". In this tree it changes only through `startFromScratch` (forgot-passcode reset with no accounts, or a
  failed key read). After a reset in the same run, `store.key` keeps the old key until restart, so recordings
  made in between become unreadable at the next start.
- **P3-6. CTR rewrites in place.** The mirror and the Off-mode copy rewrite bytes at the same offset with the
  same keystream (the MP4 trailer's patch of the `mdat` size, any muxer seek back). Only a few known header
  bytes are exposed, but the doc should say that overwrites reuse the keystream rather than imply every write is
  fresh.

## Upstream hooks, privacy and docs summary

- Hook shape: fine (see "What checks out").
- Privacy: the log is clean. The privacy gaps are P2-3 (logout) and P1-4 (wrong topic).
- Docs to correct with the fixes: the preview-send paragraph (P1-1), the resumed-round paragraph (P1-2), "a
  manifest in that state is replaced by an empty one" (P1-3; the design wants the files kept), and the key change
  sentence (P3-5).

REVIEW F10 desktop: CHANGES P1=4 P2=5 P3=6

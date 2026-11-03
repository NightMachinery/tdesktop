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

## Re-check 1

Range `1f4213134c..0b9d9f30b925ffa9dfe6e4cb166b5699ad284a03` (five commits). The gitlink is still
`a6631f8de3e813096b70dc63d01268fce009e4fd`. Only the P1 and P2 findings above, plus defects that the fixes
themselves introduce, were checked against the code at that head. Nothing was built or run.

Verdict: **CHANGES**. P1 = 0, P2 = 3. All four P1s are fixed. Two P2s are left: a remainder of P2-2 and of
the over-long manifest trigger in P1-3. One new P2 comes from the P1-4 fix.

### The original findings

- **P1-1: fixed.** The hook no longer passes `isListenState()`. `NoteRecordingStop(Listen)` now marks every open
  voice and round entry `fromListen` (`NoteStop`, then `NoteListen`, over `VoiceMirror().activeIds()` and
  `RoundCopyIds()`), so the later Cancel reads the flag from the entry. A preview send now settles its entry
  through the second pass of `MatchSent`. A Cancel after the preview keeps the file even with
  `keep_discarded_p` off (`DecideStop(..., keepDiscarded || fromListen)`), so an upload from the preview stays
  crash-safe. With the setting off, `UnclaimedListen` removes the entry once the 2-minute window has passed.
  The hook pin in `test_media_hooks.py` and the facade header are updated, and the hook is still one line.
- **P1-2: fixed.** Each encoder run of a round inherits the owner's `group`, unless the owner is closing.
  `SettleLocked` settles the whole group. The bin lists the first part only, sums the durations, and
  `RecordingBinMedia` joins the parts with `RoundMedia::Join`, offsetting each part by the end of the one before
  it, as `appendToPrevious` does. Send from the bin moves every part to `sending`, and Delete removes every part.
  A crash during a resumed round now gives one bin item.
- **P1-3: fixed for the triggers listed, except one.** `Load` now deletes strays only when the manifest is
  missing, or read `Ok`/`Empty`. Otherwise it moves the manifest to `recordings.json.bad`, adopts every
  `.ogg.prc` and `.mp4.prc` file that has no entry as a `recording` entry (so it is recovered with "Unknown
  chat", round journals included), and saves. If the rename fails, it sets `store.key = nullptr` and returns,
  so nothing is mirrored and nothing is deleted. `WriteWhole` now flushes the temporary file to disk before the
  rename (`fsync`, or `FlushFileBuffers` on Windows). The directory is per data name (`recordings-<name>` for
  `-key` profiles). The over-long manifest case is not fully fixed; see R1-2.
- **P1-4: fixed, but the fix adds R1-3.** `OriginalThread` never falls back to the history when the item has a
  topic. When the topic is not loaded, it calls `Forum::requestTopic` and shows no Send button until the box
  rebuilds. "Send to another chat" still works meanwhile.
- **P2-1: fixed.** The restart branch is gone from the mirror. A write at offset 0 while a recording is closing
  goes into the same file, so the trailer of a sub-second recording no longer opens a second entry.
  `FinishClosed` now finalises every closed entry, and `DropAfterFinalise` removes an entry still in `sending`
  that came out unrecoverable or shorter than 1 s (voice) or 200 ms (round). The stuck "Not sent" item is gone.
  A small remainder is R1-P3c.
- **P2-2: partly fixed; the rest is R1-1.** Forwarded items are now ignored (`HistoryMessageForwarded`).
  For voice, the first pass of `MatchSent` requires a direct send's mirror size to equal the document size, so a
  direct send can no longer settle an older recording.
- **P2-3: fixed.** `ForgetRecordingsAtLogout` runs from `RemoveArchiveAtLogout`. Upstream already calls that on
  both `Account::logOut` and `forcedLogOut`, so no upstream line was added. It settles every entry of that
  account index. Entries also carry the user id, and an item whose user does not match the session at its index
  shows as another account's (no Send to the original chat).
- **P2-4: fixed.** `HistoryOf` uses `peerLoaded` and then `history(peer)`, so any known peer resolves. The box
  rebuilds on `PeerUpdate::Flag::Name`, which fires when a peer's name first arrives.
- **P2-5: fixed.** `recording_cases.cpp` now runs the registry halves of the store flow in the order upstream
  calls them: a preview send (keep on and off), a preview discard, a sub-second recording, a direct send
  against an older preview discard, a voice of another size, pause and resume and send, a crashed resumed
  round, the join, logout, the adopted orphans and the over-long manifest. The Qt glue in
  `purple_recording_store.cpp` is still untested, but it now only routes into these functions.

### R1-1 (P2). A preview send settles an earlier preview discard in the same chat

This is the remainder of P2-2. The second pass of `MatchSent` takes the **earliest** `fromListen` entry of the
same account, chat and kind, and it does not check sizes.

Trace, with `keep_discarded_p` on (the default):

1. Record V1, pause into the preview, press the trash icon. `NoteListen` marks V1, then the Cancel makes it
   `discarded`, still `fromListen`.
2. Within two minutes, record V2 in the same chat, pause into the preview, press Send. Upstream sends, then
   `clearListenState` runs `hideAnimated`, and the Cancel makes V2 `discarded` and `fromListen` too. This
   happens before the upload's `itemIdChanged` arrives.
3. The first pass skips both, because neither is in `sending`. The second pass returns the earliest, V1.
   `SettleLocked(V1)` calls `RemoveLocked`.

So V1, the recording the user discarded and the bin exists to keep, is deleted. V2, which was sent, stays in the
bin as "Discarded" for 48 h, and its Send button sends it again. The new test pins this behaviour: "the
preview discard stays for a later preview send only" expects an unrelated later send to settle the older
discard.

Fix: in the second pass, first prefer an entry whose mirror size equals the document size (an untrimmed
preview send matches exactly). Otherwise take the **latest** `binned` within the window, because the Cancel of
a preview send always comes right before its `itemIdChanged`.

### R1-2 (P2). An over-long manifest still deletes the newest recordings' files

The parser no longer fails the whole array past `kMaxEntries` (4096). It now drops every entry after the
4096th without telling anyone, and `Decode` still reports `Ok`. In `Load`, that counts as clean, so
`RemoveStrays` deletes the files of the dropped entries. `Encode` writes entries in creation order, so the
entries dropped are the **newest** ones, including a crash recording that is still in `recording`. Reaching
4096 needs `bin_keep = "off"`, which is a listed choice, plus a heavy user. The test "an over-long manifest
keeps its first entries instead of failing whole" covers the parse, not this deletion.

Fix: report `Damaged` when entries were dropped, so `Load` takes the move-aside-and-adopt path, or keep the
newest entries instead of the first. Better still, never write more than the cap: evict the oldest bin items
when saving.

### R1-3 (P2, new). A deleted or unreachable topic makes the bin request it over and over

`AddItem` calls `OriginalThread(item, crl::guard(box, rebuild))` on every rebuild. For a topic that
`topicFor` cannot find, that calls `Forum::requestTopic(rootId, done)`. `Forum::finishTopicRequest` runs `done`
on success **and** on failure (`requestSomeStale`, `.fail`), and a deleted topic comes back as
`forumTopicDeleted`, so `topicFor` stays null. Then `done` rebuilds the box, the rebuild requests the topic
again, and so on. While the bin box is open and holds an item from a deleted topic (or one the account can no
longer see), the app sends `messages.getForumTopicsByID` in a loop, as fast as the server answers, and rebuilds
the whole list each time. Also, every rebuild while a request is pending adds another callback. Startup brings
a burst of `Name` updates, so one answer can fan out into many rebuilds.

The doc says a deleted topic "never gets" a Send button. That is true, but the box keeps asking for it.

Fix: skip the request when `forum->topicDeleted(item.topic)`. Request each topic at most once per box (keep the
requested ids in `BinState`), and pass the callback only on that first request.

### New P3 notes (not counted)

- **R1-P3a.** The new `fsync` in `WriteWhole` runs on every manifest save, under the store mutex: on the main
  thread for every state change, and on the capture or round recorder thread when a recording opens
  (`BeginRecordingEntry`). Right after that, the voice opener's `RecordingKey()` waits on the same mutex while
  the main thread saves the chat it just captured, which means a second `fsync`. Fine on an SSD. On a busy
  spinning disk, the start of a recording can stall for longer than before, which makes P3-1 worse. An `fsync`
  on the manifest alone, from a writer thread, would avoid it.
- **R1-P3b.** `media_recording.md` still says the file is closed "at the next recording's first write if that
  report never came", which contradicts its own new line that a write at offset 0 while closing "is its
  trailer, never a new recording". With the restart branch gone, if the capture's stopped report never arrives,
  the next recording writes over the previous file under the previous entry.
- **R1-P3c.** A sub-second recording cancelled by sliding (not sent) still leaves a junk "Discarded" item with
  `keep_discarded_p` on. Upstream resets `dataPos` to 0, so the trailer overwrites the file's start, and
  `DropAfterFinalise` only drops `sending` entries.
- **R1-P3d.** With `keep_discarded_p` off, a recording discarded from the preview sits in the bin as
  "Discarded" until the next hourly sweep (up to about 62 minutes), not 2 minutes.
- **R1-P3e.** At logout, `OfAccount` skips entries whose account is still -1, where capturing the chat failed.
  These stay in the bin, and their "Send to another chat" uses the current window's account.

RECHECK F10 desktop: CHANGES P1=0 P2=3

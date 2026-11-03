# Purple playlists: design, revision 3

Written 2026-09-30, read-only. This revises revision 2 against its two critiques (36 points) and answers the relayed request of 2026-09-30: the marker tag, whether it can sit in any chat, and re-uploading protected songs behind a confirmation. It stands alone. Everything in revision 2 that still holds is restated here.

Trees checked:
- tdesktop `dev` at `32bea43ed5`. It differs from `91b54fe2b1`, the tree revision 2 checked, only in `docs/purple/premium.md` (verified with `git diff --stat`). The purple-core submodule is pinned at `9c33c53`.
- purple-core `9c33c53`, read with `git show`. The working copy's six unpushed commits and its uncommitted G4 work are not read or relied on.
- purple-telegram-android `master` at `cb6d43f92`, unchanged since revision 2.

So every line that revision 2 verified still holds at HEAD.

How claims are labelled:
- **Verified**: read in source during this pass, with file and line at HEAD.
- **Verified earlier**: verified by revision 2 or playlist-design.md in a tree that differs from HEAD only in docs or AGENTS.md.
- **Docs**: read in Telegram's documentation (core.telegram.org), in the repositories' docs, in the owner's decision files, or in the coordinator's refactor plans and checkpoint.
- **Measured**: from a scratch Python script on synthetic data (no build).
- **H1 to H28**: hypotheses for the disposable-account spike (section 18).

**Designs this one depends on.** It references them and does not redesign them.
- **The sync-channel design** (being written, workflow wf_1bd1a0d7-a07) owns the per-account **sync chat**, the marker, startup discovery, the member warnings and chat changes. Section 8.2 lists what playlists need from it.
- **The account-settings design** (being written) owns per-account sections in settings.toml that override global keys (P10).
- **The user-control design** owns the protected-content setting (P13) and private listening (P20). **Its status at this revision:** user-control-design.md does not exist. The coordinator's checkpoint (2026-09-30, 20:10) records that the workflow stopped without a design and that the scope is back with the owner (docs). So every playlist feature that depends on it reads a flag, `protectedContentAllowed`, that the user-control layer will provide. Until that layer exists the flag reads false, which is the old rule: protected songs cannot be added and are skipped when sharing.
- **The refactor plans** (refactor-plan-desktop.md and refactor-plan-android.md). Sending records, share files and protected copies depends on desktop decision D3 and Android C3 (section 8.2).

Android paths are relative to `TMessagesProj/src/main/java/org/telegram/` unless written in full.

## What changed from revision 2

**Sync**
- **Snapshots only in v1** (critique point 2.4). Every publish carries the whole library.
  - Gone: the dirty set, delta records, the five compaction triggers with their hourly limits, and choice R1.
  - Kept: the memo, so a step that finds nothing new still costs one search and each device downloads each snapshot once.
  - Format 1 still accepts partial playlists, so deltas can come later without a format bump (choice P30).
- **Publish rule.** A step publishes when no valid record present in the sync chat equals the merged library byte for byte, compared by hash.
- **The memo no longer churns** (1.10):
  - a record the search misses is confirmed by id before its memo entry is dropped;
  - memo statuses carry the version of the code that classified them;
  - transient failures are never memoized;
  - paging runs until an empty page, or until `count` messages have been seen.
- **The automatic purge moved inside the step, after the merge**, behind four conditions (1.1, 1.3, 2.15).
  - The desktop `unixtime::updates()` gate is gone. Verified: it does not fire on a clock that is already correct.
  - The largest-stamp guard is gone.
- **Lower caps**, so records fit desktop's in-memory download (1.4, 1.12):
  - records are at most 10 MiB compressed and 32 MiB decompressed;
  - local edits are refused past 24 MiB;
  - the local file itself has no read cap.

  This is choice P29.
- **Sending.** Records, share files and protected copies go through a Purple-owned upload and `messages.sendMedia` with `silent`, not through the receipt plumbing inside upstream send code (1.5). This matches the refactor's desktop D3 recommendation.
- **Sync chats with other readers.** The first playlist publish waits for a playlist-specific confirmation, and public chats are refused by default (2.1, choice P28). The "not shown to anyone else" note appears only for private sync chats.
- **Changing the sync chat moves the records.** The old chat's records are deleted once the new chat holds a snapshot that covers them (2.3).
- **Logout.**
  - Files are deleted only when the session really ends, which Purple code observes. So a logout cancelled at one of upstream's later prompts keeps them (1.2).
  - A forced logout deletes by default (2.2, choice P27).
  - With no sync chat, logout offers setup or deletion (2.12).
- **`check_minutes` removed.** The check interval is fixed at 15 minutes (2.15).

**Library and UI**
- **Unlink, the "Keep receiving updates" switch and the imported-playlists list are gone** (2.9). Every share file still opens a preview offering Merge, Import as a new playlist, or Cancel.
- **Android playback resolves the whole playlist before Play**, and skips songs that fail. It never waits in the middle of the queue (1.14). The player sheet's changed title and rows are stated (1.15).
- **The Android player sheet.** Purple's `showMenuOptions` row also offers Add to playlist when upstream hides Save To, which it does for a protected song or the Profile music list (1.13).
- **Hook placements corrected** (1.16):
  - the desktop track-name click hook goes inside the label branch;
  - the Android listen-report guard wraps the send line.
- **Desktop share files.** One more early return, in `DocumentSaveClickHandler::onClickImpl`, so a first tap on a share file that is not downloaded yet opens it instead of saving it to Downloads (1.6).
- **Keep downloaded** means "fetched by Download all" on both clients. On desktop, the document's recorded location moves to the kept file (1.17, 2.11).

**Sharing**
- The caption carries no title and no `#` (1.8, 2.7).
- Removal tombstones go only for songs this device has already exchanged in that playlist. The confirmation discloses that the file shows when songs were added (1.19, 2.5).
- The wording is now "from you, without Forwarded from headers" (2.8).
- The map from each protected original to its copy is also applied on import, on the device that made the copies, so a round trip does not duplicate those songs there (1.7).
- Chats that charge for messages are refused in the chooser (2.6).
- Test C23 no longer claims that sources are independent of import order (1.18).

**Answers to the owner**
- The recommended tag is now `#purplechatsync` (2.13, choice P22).
- The scope of global search is relabelled as a hypothesis (1.9, H25).

**Process**
- A logging rule is added (2.16), and H19 is extended to the JNI path (1.20).
- H11 and the new H25 to H28 join the spike lists.
- The open choices are renamed P22 to P30.

## Answers to the relayed request

**Can the tag go into any chat the person chooses, and be found by a search at startup?** Yes for the first part. The second part needs a spike.
- A text message carrying the tag can sit in any chat the account can post in: the default Purple channel, a group, Saved Messages, or a private chat.
- Finding it at startup is the part to check.
  - The natural call is `messages.searchGlobal`. Its method page says only "Search for messages and peers globally", with the optional filters `broadcasts_only`, `groups_only` and `users_only` (docs, method/messages.searchGlobal).
  - Revision 2 quoted the sentence "Contains results (peers and messages) from all private/secret chats, private/public groups, private/public channels". That sentence describes what the Chats search tab should show. The same passage says that tab shows "max one message per peer" (docs, api/search).
  - So whether one global search finds a marker in every kind of chat, including Saved Messages and archived chats, is hypothesis H25. The sync-channel design owns discovery and its fallback.
- Three rules keep a marker safe, whichever chat holds it:
  - Only a marker the account itself sent counts (the `out` flag, H11). A stranger who types the tag into a shared group cannot redirect anyone's sync.
  - The marker is a text message.
  - A public chat is a poor home for it. `channels.searchPosts` searches posts "from public channels (including those we aren't a member of)" and takes a hashtag (docs, api/search). So a marker or a record in a public channel can be found by anyone. Playlist records refuse public chats by default (P28).
- Playlist records never need the global search. Once the sync chat is known, the playlist step searches only inside it (section 8.3).

**Which tag?** `#purplechatsync`.
- **Why not `#purplesyncchat`.** The shipped settings rule accepts any non-forwarded document whose caption *contains* `#purplesync` (verified, core `9c33c53`, `purple_sync_inventory.cpp:64-70`, with the tag defined at `:52-54`). `#purplesyncchat` contains it. It would stay safe only while two rules held forever: the marker is text, and every later predicate matches whole tags.
- `#purplechatsync` contains neither `#purplesync` nor `#purpleplaylists`, so neither rule carries any weight.
- It is letters only, so the way Telegram tokenizes underscores never matters. Nobody else is likely to use it, which keeps any global search result small.
- H13 checks that it, and `#purpleplaylists`, each match only themselves.
- The existing tags stay: `#purplesync` for settings records (shipped) and `#purpleplaylists` for playlist records. Share files carry no tag at all.
- This is choice P22, shared with the sync-channel design.

**Re-uploading protected songs, with a confirmation dialog.** Section 10.5 designs it.
- The dialog says:
  - how many songs are affected;
  - how much data must be downloaded and uploaded;
  - that the copies will not be protected.

  Its buttons are **Upload copies**, **Skip these songs** and **Cancel**.
- It depends on the user-control protected-content setting. That design stopped without output, as the dependency list above records. Until the owner settles its scope, sharing ships with "skip these songs, and say why". G-5 is the only phase item that waits for it.
- Two facts for that decision:
  - Telegram's content-protection page asks clients to treat protected messages as if "forwards, downloads, copying, screenshots must be disabled" (docs, api/content-protection, as quoted in user-control-research-terms.md).
  - The server sees a copy as a fresh upload.

  The terms research also records that enforcement falls on the api_id owner. The dialog makes the choice explicit to the person, but it does not change those facts.

## 0. Summary

- **A playlist** is a named, ordered list of songs the account can already see in its chats. Each song is stored as:
  - its Telegram document id;
  - a source message that carries it, plus up to three fallback sources;
  - a few display fields;
  - the stamp at which it joined the playlist.

  No audio, access hash or file reference leaves the device.
- **Ownership.** Each Telegram account has its own library on each device, and it syncs through that account's sync chat.
- **Merging** keeps, for every field, the value with the larger stamp, and never asks a question. Songs added on two devices are both kept.
- **Sync** is on by default and automatic:
  - after edits (60 s after the last edit, and at most 10 minutes after the first);
  - at app start and when Playlists opens, at most every 15 minutes;
  - on **Sync now**.

  A step does the following:
  - searches the sync chat for records;
  - downloads only records this device has not merged before;
  - merges them;
  - runs the automatic purge when it is safe to;
  - publishes a snapshot of the whole library if no record already equals it;
  - deletes records that another record covers.

  The steady state is one record, and one search request per check.
- **Deleting** has two levels. Delete puts a playlist in Recently deleted, and Delete forever purges it. A playlist left in Recently deleted for 30 days is purged automatically.
- **Sharing** forwards the songs to a chat, then sends a share file.
  - The songs arrive from the sharer, with no "Forwarded from" header and no caption.
  - A Purple recipient opens the file as a playlist, and a later file merges into it after a preview.
  - Protected songs are re-uploaded only after a confirmation, and only once the user-control setting exists.
- **Offline listening:** **Download all** per playlist. With **Keep downloaded** on, which is the default, those files are kept past Keep Media and the cache limits.
- **Playback** uses Telegram's own players in playlist order, through guarded hooks.
- **Privacy.**
  - Private listening is the default (user-control design).
  - Records never hold access hashes, file references or the account's own id.
  - Share files never name the sharer's source chats or message ids.
  - Playlist records do not go to a sync chat with other readers until the person confirms, and never to a public chat by default.

## 1. Terms

- **Library**: all playlists of one Telegram account on one device, kept as one canonical JSON document.
- **Sync chat**: the chat the sync-chat layer names for an account. That is the default Purple channel, or a chat the person chose.
- **Other readers**: people other than the account who can read the sync chat. The sync-chat layer reports one of three values: none, members (with a count), or public.
- **Song entry**: one song in one playlist, keyed by its document id. Its value is a register holding either a source value, or `false` for a removed song.
- **Source value**: a typed chat token, a message id, an optional channel username, up to three **fallback sources**, display fields, and `since`.
- **`since`**: the stamp at which the song last joined the playlist.
  - A relink or an Undo keeps it.
  - A fresh add, or a re-add after removal, sets a new one.
- **Register**: a pair `[value, stamp]`. A merge keeps the one with the larger stamp.
- **Stamp**: an integer number of milliseconds, from 1 to 2^52. Local operations write stamps. Merges and share imports copy stamps and never make new ones, except for the restore an import performs.
- **Partial playlist**: a playlist in which some of `title`, `deleted`, `order` or `songs` are absent. An absent register is the bottom element of the merge. v1 never writes partial playlists, but it accepts them (section 7.3).
- **Join**: the merge of two libraries, partial or not (section 7.7). It is commutative, associative and idempotent.
- **Contains**: library O contains library R when join(O, R) equals O byte for byte, in canonical form.
- **Covers**: record O covers record R when O's library contains R's, and either the two libraries differ or O has the larger message id. Covering is a strict order.
- **Record**: a document named `Purple playlists.json.gz`, captioned with `#purpleplaylists`, and sent by the account itself into its sync chat. Its bytes are a gzip of a canonical library.
- **Present records**: the records the current step knows to exist in the sync chat. That is the search results, plus memo entries the search missed but a fetch by id confirmed.
- **Memo**: the list of records this device has already classified. Each entry is keyed by message id and document id. It holds the record's status, the version of the classifier, and for valid records the hash of the canonical bytes and a cached copy of the compressed bytes.
- **Playlist step**: one sync pass: search, read the new records, merge, purge, publish and prune (section 8.4).
- **Share file**: a small gzip file sent after the forwarded songs. It holds one playlist without any source references (section 10.6).
- **Exchanged set**: for one playlist on one device, the document ids this device has sent in a share of it, or adopted from a share file of it. It is local and never synced (section 10.6).
- **Copy map**: on one device, the map from a protected original's document id to the document id of the copy that device uploaded, for each destination chat. It is local.
- **Playlist mode**: the player is stepping through a Purple playlist, not through the playing song's chat.
- **Local sync file**: a small per-account file on the device. It holds status, the memo and bookkeeping (section 7.1), and is never posted.

## 2. What a playlist is and holds

A playlist holds:
- **an id**: `pl-` followed by 16 lowercase base32 characters, which is 80 random bits. The id also identifies the playlist across people when it is shared (section 10.2);
- **a title register**: 1 to 64 code points on one line, trimmed, with no control characters;
- **a deleted register** (`true` or `false`), so that Delete can be undone;
- **an order register**: document ids in the order the person last arranged them;
- **songs**: a map from document id to a register holding either a source value or `false`.

A source value holds:
- `peer`: a typed chat token (section 3.1);
- `msg`: the message id;
- `un`: the channel's public username. It is present only when the channel had one at the time of adding;
- `alt`: 0 to 3 earlier sources, newest first. Each has a `peer`, a `msg` and an optional `un`. None equals the primary source, and none repeats another (P11);
- `meta`, the display fields:
  - `title`: the audio attribute's title, or the file name when that title is empty;
  - `perf`: the performer;
  - `dur`: the duration in whole seconds.

  Core cuts each string to 96 code points, so both clients produce the same bytes from the same document.
- `since` (section 1).

**What a playlist does not hold:** audio, cover art (each device derives it from the document thumbnail), play counts, notes, folders or rules.

**What stays on the device and is never synced:** availability, downloads, the queue, the shuffle order, playback positions, recent use, share history and the copy map.

**List order (P19).**
- Playlists are listed by the most recent use on this device, then by title using core's case-folded comparison, then by id.
- **Use** means opening the playlist, playing from it, or adding to it.
- Recent-use times live in the local sync file and are never synced, so two devices may order the list differently.
- Every Add menu uses the same order. A song appears at most once in a playlist.

**Ownership.**
- Each signed-in account has its own library. The Add menu in a chat of account B adds to B's playlists.
- The Playlists screen shows the active account's library.
- So a library never holds a reference its own account cannot resolve, and one account's chat ids never reach another account's sync chat.

## 3. How a song is referenced

### 3.1 The source

The chat token `peer` is one of:
- `self`: Saved Messages. The account's own user id is never written, wherever the record itself lives.
- `user:<id>`: a private chat or bot chat, holding the other party's id.
- `chat:<id>`: a basic group, including the pre-migration half of a migrated group.
- `channel:<id>`: a channel or supergroup, including forum topics and channel direct messages.

Ids are bare decimal ids. `msg` runs from 1 to 2^31 - 1.

**Why the type is required.** Docs (through the research maps of playlist-design.md): peer id sequences overlap across types, and basic groups and supergroups number their messages separately. Purple's `ChatKind::Group` merges those two kinds, so it cannot be reused here.

**Why the reference works on another device of the same account.**
- Docs (Telegram's updates documentation): private chats and basic groups share one message-id sequence per account, identical on every session of that account. Channel message ids are the same for everyone.
- So a pair of peer and message id means the same message on every session of one account.
- It does not mean the same message in another account. That is one reason each account has its own library, and the reason share files carry no message ids (section 10.6).

**Never stored:** access hashes (bound to a login session), file references (which expire), local paths, and the account's own user id.

**The `un` hint** lets a device that has never seen a public channel resolve it with `contacts.resolveUsername`. It is public data, and the channel id beside it is already in the record.

### 3.2 Why the document is the identity

- **Forwarding keeps the document id.** A song added from a channel and again from Saved Messages is one entry.
- **Relinking needs no new concept.** Adding the same document from any chat relinks the entry to that message and keeps its place. The old source becomes a fallback (section 4.1).
- **The cost.** A re-upload of the same song gets a new document id, so it becomes a second entry. That includes a protected song's shared copy (section 10.5). The person removes the duplicate by hand.

### 3.3 Which songs qualify

A song can be added when every one of these holds:

- **The document has an audio attribute without the voice flag.**
  - On desktop that is `DocumentData::isSong()` (verified earlier, `data_document.h:192`).
  - On Android the helper checks the attributes directly. That set is inside `MessageObject.isMusic()`, which `setPlaylist` keeps (verified earlier, `MediaController.java:2862-2872`).
- **The message is a regular server message.** That rules out scheduled messages, unsent local messages, sponsored messages, stories, upstream profile-music items and secret chats.
- **The message's main media is that document**, not a poll's or a web page's.
- **The message has no self-destruct timer.**
- **Protection (P13).**
  - While `protectedContentAllowed` is true, songs from protected chats, and protected messages, qualify.
  - While it is false, they do not. That is the old rule, and the state until the user-control layer exists.
  - Songs already in a playlist stay and keep playing when the flag turns false. Only new adds and new copies for sharing are refused.

**Core holds this rule** as a pure function, `QualifySong`. It works over plain fields the client fills in, plus the protected-content flag, so both clients decide the same way.

**Where P13 meets upstream's own protection gates.** Several Purple entry points sit behind gates that upstream checks before calling them. The user-control layer has to lift these for P13 to reach playlists. This design assumes it lifts protection at its root, so that all of them open together. If it lifts gates one at a time, these are the ones playlists sit behind:
- **Desktop, single song.** All three callers of `AddSaveDocumentAction` are gated:
  - the chat menu, on `!hasCopyMediaRestriction(item) && !ItemHasTtl(item)` (verified, `history/history_inner_widget.cpp:3111-3113`);
  - the section-list overload, which returns early on `hasCopyMediaRestriction(item) || ItemHasTtl(item)` (verified, `history/view/media/history_view_save_document_action.cpp:117-124`);
  - shared media, on `_provider->allowSaveFileAs(item, document)` (verified, `info/media/info_media_list_widget.cpp:1315`).
- **Desktop, multi-select.** `Menu::AddDownloadFilesAction` is called only under `!hasCopyRestrictionForSelected()` (verified, `history/history_inner_widget.cpp:3407`, `:3714`) and returns early on `list->hasCopyRestrictionForSelected()` (verified, `history/view/history_view_context_menu.cpp:997-1000`).
- **Android player sheet.**
  - "Save To" is added only when `!noforwards` (verified, `ui/Components/AudioPlayerAlert.java:2878`, `:2977`), and it is skipped entirely for the Profile music list (`isMyList()`, verified `:2877`).
  - A row's option button is null when `noforwards` (verified, `:2539`).

  Purple's own `showMenuOptions` row covers both cases (section 6.2), so this gate needs no lifting.
- **Android chat menu and action bars.** Purple adds its own rows there, and `QualifySong` decides, so no upstream gate applies.

Timer messages stay out, because their source vanishes by design. If the user-control design ever keeps timer media locally, referencing those kept copies is a follow-up.

### 3.4 Resolving on a device

Resolution runs when a playlist opens (visible rows first, in batches, then the rest in the background), before Play on Android, ahead of the playing song on desktop, and before a share. Results are kept in memory only.

1. **Local first.** Desktop uses `Data::Session::message(peerId, msgId)`. Android uses `MessagesStorage.getMessage(dialogId, msgId)`.
2. **`self`, `user` and `chat` sources.** Request `messages.getMessages` with bare message ids, in batches: on desktop through `ApiWrap::requestMessageData`, on Android in batches of up to 100. This needs no peer access hash (H1).
3. **`channel` sources.** Request `channels.getMessages`, which needs the channel's access hash on this device:
   - if the channel is known, request it directly;
   - if the channel is unknown and `un` is present, resolve the username first;
   - otherwise show "Open the channel once on this device", and retry when the dialog list finishes loading (H2).
4. **Check the document.** The fetched message must carry this entry's document id. Otherwise the source is "Changed".
5. **Fallbacks (P11).** If the primary source ends in Deleted, No access, Changed or Restricted here, try each `alt` source in order with the same steps. The first that resolves is used for playback and sharing.

Once the message is loaded, file-reference refresh works exactly as it does from a chat: desktop's `ApiWrap::refreshFileReference`, and Android's `FileRefController` (verified earlier).

### 3.5 Unavailable songs

A row shows one of these states:
- **Resolving**;
- **Deleted**: the message is gone;
- **No access**: the channel was left, is private (`CHANNEL_PRIVATE`), or is unknown here;
- **Changed**: the message now carries another file;
- **Restricted here**: a platform restriction applies;
- **Couldn't load**: a network failure, retried the next time the playlist opens;
- **Playing an earlier copy**: the primary source failed and a fallback resolved. The row menu offers **Use this copy**, which relinks the song to that fallback.

Unavailable rows stay in place, greyed, with their saved title and performer and a one-line reason. Playback and sharing skip them. Nothing is removed or relinked automatically, and an unavailable song is never a sync error. To relink a song, add it again from any chat where it still exists, or pick **Use this copy**.

## 4. Creating and editing

### 4.1 Operations

**How operations work.** Every operation is a pure core function of (library, operation, time in milliseconds, entropy). It returns one of:
- the new library plus an **undo list**, which records every register the operation overwrote and its prior value;
- a typed refusal.

Each operation takes a stamp s from its playlist's clock (section 7.6). The client saves the library file, records `unsynced_since` if it is not already set, and restarts the automatic-sync timer (section 8.7).

**The operations:**

- **Create(title)**: a new id, with `title=[title, s]`, `deleted=[false, s]`, `order=[[], s]` and no songs.
- **Rename(id, title)**: `title=[title, s]`.
- **Delete(id)**: `deleted=[true, s]`.
  - The songs stay inside, hidden.
  - The playlist appears in Recently deleted with "Deletes forever on <date>", where the date is s plus 30 days.
- **Restore(id)**: `deleted=[false, s]`.
- **Delete forever(id)**: allowed only for a deleted playlist. It adds the id to `purged` and drops the playlist. It takes no stamp.
- **AutoPurge(library, now, gate)**: runs Delete forever on every playlist whose rule in section 7.9 has come due and whose conditions hold. It takes no stamp. Only the playlist step calls it.
- **Add(id, songs in order).** For song i with value v:
  - If the song is absent or removed, write `[v with since = s + i, s + i]`. Consecutive stamps keep an album, or a selection, in its given order at the end of the playlist.
  - If the song is a member whose primary source has the same `peer` and `msg`, do nothing. The client says "Already in <title>".
  - If the song is a member with a different primary source, this is a **relink**.
    - Write `[v', s + i]`, where v' is v with the old `since`.
    - Its `alt` is the old primary followed by the old `alt`, with any source equal to v's primary removed, cut to three.
    - The song keeps its place, because display order uses `since` (section 7.8).
- **Remove(id, documents)**: `[false, s]` for each. The fallbacks go with the value.
- **Reorder(id, documents)**: `order=[documents, s]`, where documents is the full visible list in its new order. Core refuses a list that is not a permutation of the visible songs.
- **CopyPlaylist(playlist value)**: a Create with the old title plus " (copy)", followed by an Add of the old playlist's visible songs in display order. Each song keeps its primary source and fallbacks. **Keep a copy** uses it, and so does **Import as a new playlist** (section 10.7).
- **ImportShare(share, resolved songs, now)**: section 10.7.

Only Reorder writes the order register. That is what lets a relink or an Undo on one device coexist with a reorder on another, instead of discarding it.

### 4.2 Undo

Undo writes the undo list back, using one fresh stamp:
- A song that was absent before goes to `false`, and so does a song that was removed before.
- A relinked song gets its old source value back, including its old `since` and old fallbacks.
- A removed song gets its old source value back, so it returns to its place, provided no reorder has happened since.
- A song the operation left alone stays untouched. So undoing "Add album" never removes songs that were already in the playlist.
- Undo of Delete is Restore. Undo of Create is Delete forever of the playlist, which is still empty.
- Undo of an import restores every register the import wrote.
- Delete forever and AutoPurge have no undo. The confirmation for Delete forever says so.

### 4.3 The Add menu

Both clients list the account's playlists, most recently used first, with a check mark on each one that holds the song. The menu only adds:
- **An unchecked playlist** runs Add and shows "Added to <title> · Undo".
- **A checked playlist whose primary source is this message** shows "Already in <title>".
- **A checked playlist whose primary source is another message** runs a relink and shows "<title> now plays this copy · Undo". The previous copy stays as a fallback.

Songs are removed only inside the playlist view. The client passes core no availability flag, because whether the old source still works on this device makes no difference to a relink.

**The first playlist.** When the account's first playlist is created, from this menu or from the Playlists screen, the first-run bulletin appears right there (section 8.10).

### 4.4 Limits and sizes

- **Local operations**, imports included, refuse to grow the canonical library past **24 MiB**. The refusal reads "Playlists are full. Delete forever deleted playlists, or remove songs, to make room."
- **A merge is never refused.** A merged library above the record caps can still be edited downward and synced by reading, but this device publishes no snapshot until it fits again (section 8.8).
- **The local file has no read cap**, so a library that grew past 32 MiB by merges still opens after a restart (section 7.4).

**Measured sizes.** These are canonical compact JSON in this format, from synthetic libraries with 17% removed songs, half Persian text and half with `un`:
- 500 entries: 90,353 bytes, or 24,899 gzipped (this pass).
- 2,000 entries: 365,447 bytes, or 97,917 gzipped (this pass).
- 7,200 entries in 40 playlists: 1,323,727 bytes, or 354,207 gzipped (this pass). Revision 2's script gave 1,288,431 and 313,032. That is about 180 bytes per entry, and a gzip ratio of 3.6 to 4.1.
- At about 184 bytes per entry, 24 MiB holds about 137,000 entries and 32 MiB about 182,000.
- A 24 MiB library gzips to about 6 to 7 MiB, and a 32 MiB one to about 8 to 9 MiB. Both fit the 10 MiB record cap unless the text compresses worse than a ratio of about 3.2.
- A bomb: 64 MiB of one repeated byte gzips to 65,251 bytes (revision 2). A size check on the compressed file alone is therefore no guard (section 7.4).

A fallback source costs about 40 to 50 bytes, and exists only on relinked songs.

If two devices each create a playlist named "Gym" while out of sync, they end up with two playlists named "Gym". The person combines them by hand, because the design does not merge playlists by name.

## 5. Playback and queue

### 5.1 Shared rules

- **Starting.** Pressing **Play**, **Shuffle** or a song row builds a queue: a snapshot of the visible, playable songs in display order. Edits apply the next time Play is pressed.
- **Global modes.** Repeat (none, one, all) and order (normal, reverse, shuffle) stay the existing global settings: `Core::Settings` on desktop and `SharedConfig` on Android. Shuffle plays a random order that starts with the current song.
- **Resolution and failures.** A song that cannot be resolved is skipped. It is never waited for in the middle of the queue.
  - Desktop resolves the next few songs, with fallbacks, while the current one plays.
  - Android resolves the whole playlist before Play (section 5.3).
- **Leaving playlist mode.** Playing any song from outside the playlist ends playlist mode, and the player returns to Telegram's usual chat queue.
- **Persistence.** The queue lives in memory only, like upstream's queue.
- **Listening reports (P20).** Private listening is owned by the user-control design and is on by default. While it is on, no song, from a playlist or a chat, sends `messages.reportMusicListen` (H4). The candidate hook sites:
  - **Desktop:** `MusicListenTracker::report` (verified, `media/player/media_player_listen_tracker.cpp:67-72`). One early return goes after its three `base::take` lines, so the tracker still resets.
  - **Android:** the tracker's `report()` (verified, `messenger/MediaController.java:6992-7005`). A guard condition goes on the send line at `:7002`: `if (!PurplePrivacy.privateListening()) ConnectionsManager...sendRequest(req, null);`. The resets at `:7004-7005` then still run, so listening gathered while private listening is on is never reported later if the switch turns off in the middle of a song. A return placed right after `if (audio == null) return;` would skip those resets.

  While private listening is off, playlist playback reports exactly as chat playback does, because every song is a real, loaded message.

### 5.2 Desktop

`Media::Player::Instance` has no explicit queue. All navigation goes through three members (verified earlier): `moveInPlaylist` (`media_player_instance.cpp:554`), `nextAvailable` (`:763`) and `previousAvailable` (`:746`). OS media controls, media keys, the dock menu and the Windows taskbar call the same members. Repeat One acts before `moveInPlaylist` is reached, and so does the `disable-autoplay-next` option.

**The new `purple/purple_playlist_player.{h,cpp}`** keeps:
- the session and the playlist id;
- the snapshot of document ids;
- the current position and the shuffle order;
- the **started id**: the `AudioMsgId` it last started.

It plays each song with `instance()->play(AudioMsgId(document, sourceFullMsgId))`, so streaming and file-reference refresh behave as they do from a chat. It records the started id **before** calling `play()`, because `play()` can fire `playlistChanges` synchronously. If the next song is still resolving when a move arrives, the move returns true and the song starts once it resolves. If it fails, the move skips on.

**Hooks:**
- **`media/player/media_player_instance.cpp`: one include and three guarded early returns.**
  - At the top of `moveInPlaylist`, for the Song queue only: `if (const auto moved = Purple::PlaylistMove(data->current, delta, autonext)) { return *moved; }`.
  - At the top of `nextAvailable` and `previousAvailable`, for the Song type: return `Purple::PlaylistAvailable(current, +1 or -1)` when it has a value.

  Each Purple function returns `std::nullopt` unless the player's current track equals the started id. So playlist mode ends by itself the moment anything else plays, pause and resume do not end it, and the Voice queue is never touched.
- **`media/player/media_player_panel.cpp`: one include and one early return** inside the `peer` lambda of `Panel::refreshList()` (verified earlier, `:235-265`): `if (Purple::PlaylistClaims(current)) return nullptr;`. A null peer destroys any list already built, and the panel stays closed.
- **`media/player/media_player_widget.cpp` (P7): one include and one guarded early return inside the label branch of `Widget::mouseReleaseEvent`**, right after upstream's `if (_labelsOver != downLabels) { return; }` (verified, `:476-479`): `if (Purple::PlaylistTrackClicked(_type)) { return; }`.
  - While the playing song belongs to a playlist, a click on the track name opens that playlist's box, scrolled to the playing row.
  - A return at the top of the function could not tell a click on the track name from any other release, because `_labelsDown` is private and has to be taken first.
  - Upstream's own action in that branch, which shows the item, runs only for Voice or for a song from another session (verified, `:480-481`), so nothing upstream is lost.

**Known v1 limits:**
- Upstream still builds its 32-id window around each song's own chat.
- With Shuffle on, upstream also pages that chat's music ids to build its own shuffle list (verified earlier, `:377`, `:960`). Playlist mode never uses that list. Acceptance D2 counts these requests.
- The chat does not scroll along with playlist songs.

### 5.3 Android

`MediaController` keeps one process-wide queue, and three of its behaviours break playlist order:
- It re-sorts the queue by message id on every set (verified earlier, `sortPlaylist`, `:2897`).
- A late `musicDidLoad` for the playing song's chat merges that chat's songs into the queue (verified earlier, `:1877-1899`).
- `loadMusic=false` sets `forceLoopCurrentPlaylist` (verified earlier, `:2854`), which overrides both Repeat One and stop-at-end (verified, `:2951`, `:2964`).

**Edits to `messenger/MediaController.java`**, each marked `// Purple:`:
- **One import.**
- **One guard condition** in the same-object shortcut at the top of the five-argument `setPlaylist` (verified earlier, `:2847`): `if (playingMessageObject == current && !PurplePlaylistPlayer.replacingQueue())`.
  - This is the rule's "smallest extension point". No hook of call or return shape can skip that shortcut, and Purple cannot reach the private queue itself.
  - The alternative is to restart the current song through `cleanupPlayer`. That needs no edit at all, but it restarts a song the person is listening to.
- **One early return** at the top of `sortPlaylist()` (`:2897`): `if (PurplePlaylistPlayer.keepsOrder(this)) return;`.
- **One early return** at the top of the `musicDidLoad` branch (`:1877`), with the same condition. Verified earlier: nothing follows the branch chain in `didReceivedNotification` (`:1826-1993`), so returning there skips only that branch.

**Nothing else in the file changes.**
- **No field.** `PurplePlaylistPlayer` holds the state. `keepsOrder(mc)` is true while one of two things holds:
  - Purple's own `setPlaylist` call is running;
  - `mc.getPlaylist()` (public, verified earlier `:4402`) still holds exactly the objects Purple passed, in the same positions.

  When upstream sets another queue, the objects change and the flag lapses by itself, so `clearPlaylist()` needs no hook.
- **No overload, and no edit to the forced-loop line.** Purple calls the existing five-argument overload with `loadMusic = true` and its own `PlaylistGlobalSearchParams`, whose `endReached` is `true`. The class, its constructor and `endReached` are public (verified earlier, `:6808-6830`). So:
  - `forceLoopCurrentPlaylist` stays false (`:2854`), and Repeat One and stop-at-end work;
  - the `loadMusic` branch generates a class guid instead of loading the chat's music (`:2888-2893`);
  - `loadMoreMusic()` returns without a request, because `endReached` is set and the global-search branch returns (verified earlier, `:2733-2826`).

  H24 checks the whole path.
- **The params change the player sheet, and that is accepted.** `currentPlaylistIsGlobalSearch()` returns `playlistGlobalSearchParams != null` (verified, `MediaController.java:6831-6833`). While it is true:
  - the sheet does not replace its "Music" title with the chat's name (verified, `ui/Components/AudioPlayerAlert.java:1345`, `:1352`);
  - rows use `AudioPlayerCell.VIEW_TYPE_GLOBAL_SEARCH` (verified, `:2497`), which appends the song's chat after the performer (verified, `ui/Cells/AudioPlayerCell.java:139-143`).

  That suits a playlist whose songs come from many chats. Acceptance A2 checks it on screen. No hook is needed.

**How the order comes out right.**
- `setPlaylist` adds the given list from its last element to its first (verified earlier, `:2862-2872`).
- With the sort skipped, the stored array is the playlist reversed.
- "Next" moves toward index 0, and the player sheet lists the queue from the last index down.

So passing the playlist in display order gives the right next and previous, and a sheet that shows song 1 on top. Shuffle uses upstream's `buildShuffledPlayList`.

**How a queue is built (the model).** Upstream's queue holds only real `MessageObject`s, and auto-advance plays whichever object comes next (verified, `playNextMessageWithoutOrder` and `traversePlaylist`, `MediaController.java:2948-2975`). So an unresolved song cannot sit in the queue.
- Resolution starts when the playlist opens: visible rows first, then the rest in the background.
- **Play** waits until every visible entry has resolved or failed, and shows "Preparing 120 of 420 · Cancel" while it waits. A playlist that has been open for a few seconds is usually ready at once.
- Failed songs are left out of the queue, as on desktop.
- `messenger/purple/PurplePlaylistPlayer.java` then calls `setPlaylist` once, with `replacingQueue()` true around the call.

**The song that is already playing.** When an entry is the song already playing (`isSamePlayingMessage` against `getPlayingMessageObject()`), the player puts that exact playing object into the list instead of a fresh copy. Verified earlier:
- `playMessage` returns early on `isSamePlayingMessage` without replacing `playingMessageObject` (`:3624-3631`);
- `MessageObject` overloads only `equals(MessageObject)` (`MessageObject.java:12212`), so `indexOf` compares object identity;
- `setPlaybackOrderType` clears the playlist and stops playback when `indexOf(playingMessageObject)` is -1 (`:4263-4275`).

With the substitution and the guard condition, the song resumes instead of restarting, and turning Shuffle off keeps it playing.

Show in chat, Forward and Save To all work in the player sheet, because every item is the real message.

**Known limits:**
- The Android Auto browse tree does not list playlists.
- A killed process forgets the queue, but the playlist remains.
- `playlistMap` is keyed by message id alone. With loads off it is never used for merging, so two songs with equal ids from different chats both play (A2 checks this).
- A large, channel-heavy playlist can take a few seconds to prepare the first time.

### 5.4 Pinned music, in chat order

Unchanged from revision 2. Neither pinned-music engine produces chat order today (verified earlier):
- **Desktop.** The job queues each song straight into `document->save` as it discovers it (`purple_pinned_music.cpp:160-172`). The order is the newest pinned anchor first, then that anchor's neighbours.
- **Android.** The job fills a `LinkedHashMap` in the same discovery order (`PurplePinnedMusic.java:376`). The list screen plays those rows through `setPlaylist(..., false, null)` (`PurplePinnedMusicList.java:290-301`).

**The fix, in Purple files only:**
- **Chat order** is a core sort key: songs from the migrated-from basic group first, then message id ascending.
- **Desktop.** The job gains a collect mode that records ids, applies `QualifySong` and counts skipped audio. The ids are then sorted into chat order and saved as a new playlist named "<chat> pinned". The toast reads "Saved 42 songs as “<chat> pinned” · Open", plus "3 audio files skipped (not tagged as music)" when any were skipped.
- **Android.** **Save as playlist** in `PurplePinnedMusicList` does the same. Playing the pinned list sorts the rows into chat order and passes them through the same ordered path as playlists (section 5.3), which fixes migrated groups.

## 6. UI entry points

### 6.1 Desktop

**Add to playlist.** One call at the top of the first `HistoryView::AddSaveDocumentAction` overload (verified, `history/view/media/history_view_save_document_action.cpp:36`): `Purple::AddPlaylistActions(addAction, item, document, controller);`.
- It sits above upstream's early return for the plain Save item. It inherits the gates of the overload's three callers, which section 3.3 lists: the main chat, the section lists, and shared media, which includes the player's hover list.
- It adds:
  - **Add to playlist**: a submenu that behaves as in section 4.3, with **New playlist...** at the bottom;
  - **Add album to playlist**, for a song in an album of two or more songs;
  - **Open as playlist**, for a share file (section 10.7).
- Code: `purple/purple_playlists_menu.{h,cpp}`.
- **A known limit, accepted.** While a song is downloading, both the chat menu and the shared-media menu offer only Cancel and return early (verified earlier: `history_inner_widget.cpp:3080`, `info_media_list_widget.cpp:1281`). D1 records whether the playing song reports `loading()`.

**Multi-select (P5 c, and choice P24).** the owner asked for the entry in the multi-select bars. Desktop's selection bar has no place that a hook of the rule's shape can fill:
- `TopBarWidget` holds its selection buttons as members (verified, `history/view/history_view_top_bar_widget.h:234`);
- each button is constructed, wired and laid out in several places (verified, `history_view_top_bar_widget.cpp:122`, `:137`, `:156-157`, `:1178-1210`, `:1350`, `:1527`).

So by default the entry goes in the selection context menus, which is a departure from the owner's answer and is reported to him as choice P24:
- one guarded call at the top of each `Menu::AddDownloadFilesAction` overload (verified earlier, `menu/menu_item_download_files.cpp:235`, `:261`): `Purple::AddSelectedToPlaylistAction(menu, window, items);`. These overloads are called from the chat's selection menu (verified, `history/history_inner_widget.cpp:3407`, `:3714`) and from the section lists (verified, `history/view/history_view_context_menu.cpp:994-1006`, `:1132`);
- one delegating call in shared media's selection menu, before **Clear selection** (verified, `info/media/info_media_list_widget.cpp:1376-1381`).

The item reads **Add N songs to playlist...**. It appears when the selection holds at least one qualifying song. Other selected items are skipped, and the toast says how many. It opens the same picker as a single song, and adds the songs in chat order.

**Top-level entry (P8 b).** One delegating menu row in `window/window_main_menu.cpp`, next to the existing Purple preset entry (verified earlier, `:741-749`). It is shown while `[playlists] menu_item_p` is true, which is the default. It opens `Purple::PlaylistsBox`, which shows:
- playlists, recently used first, each with its song count and a marker on the one that is playing;
- **New playlist**;
- **Recently deleted**, shown when any deleted playlist exists, with "Deletes forever on <date>", **Restore** and **Delete forever**;
- the sync line (section 8.10);
- a note that depends on the sync chat:
  - with no other readers: "Profile music is Telegram's public list on your profile. Playlists here are not shown to anyone else unless you share them.";
  - otherwise: "Playlists sync through <chat>, which <N> other people can read."

**PlaylistBox**, for one playlist:
- **Play** and **Shuffle**, and **Download all** (section 11);
- rows showing title, performer, duration, state and a downloaded mark, where a click plays from that row;
- a row menu with **Remove** (with an Undo toast), **Show in chat**, **Use this copy** (only on rows playing an earlier copy) and **Add to playlist...**;
- a box menu with **Share...** (section 10), **Rename** and **Delete** (with an Undo toast);
- drag to reorder, on **one painted widget** that paints only the visible range. `Ui::VerticalLayoutReorder` needs one widget per row (verified earlier, `vertical_layout_reorder.h:46-47`), which does not scale.

Code: `purple/purple_playlists_box.{h,cpp}`. A box avoids a new `Info::Key` variant.

**Pinned music:** **Save as playlist** sits beside Download (section 5.4).

**Opening a share file.** Two guarded early returns:
- **An undownloaded file.** A tap on it reaches the save handler, not the open handler. The document view sets `result.link = _openl` only when the file is loaded or playable, and `_savel` otherwise (verified, `history/view/media/history_view_document.cpp:1450-1454`). The save handler would then write the file to Downloads or open a save dialog (verified, `data/data_file_click_handler.cpp:74-115`). So one early return goes at the top of `DocumentSaveClickHandler::onClickImpl` (verified, `:140`): `if (Purple::OpenPlaylistShareOnTap(document(), context())) { return; }`.
  - It acts only for a document named `*.purpleplaylist` of at most 4 MiB.
  - It does not touch `Save`, so "Save As..." still saves the file.
- **A downloaded file.** One early return in `Data::ResolveDocument`, right after its null check (verified earlier, `data/data_document_resolver.cpp:239`): `if (Purple::OpenPlaylistShare(controller, document, item)) { return; }`.

Purple reads the file through the reader's in-memory path (section 8.2), never into Downloads.

**Strings** are literals, as the sync UI already uses, so `lang.strings` is not edited.

### 6.2 Android

**Chat message menu** (`ui/ChatActivity.java`).
- One call at the very end of `fillMessageMenu` (verified earlier, `:46020`): `PurplePlaylistPicker.addMenuOption(this, selectedObject, icons, items, options)`. It inserts **Add to playlist** right after Save when Save is present, and otherwise before Delete. For a share file it inserts **Open as playlist**.
- One delegating `case PurplePlaylistPicker.OPTION_ADD_TO_PLAYLIST:` in `processSelectedOption` (verified earlier, `:33632`), and one for `OPTION_OPEN_SHARE`.
  - The constants are `public static final int` values, 1001 and 1002, inside the Purple class, so ChatActivity gains no constant.
  - Upstream's last option is 116 (verified earlier, `:1255`).

**Chat multi-select (P5 c).** Three one-line hooks:
- one call after the `save_to` item is created in `createActionMode` (verified earlier, `:10371`), which adds a Purple item;
- one call next to `saveItem.setVisibility(...)` in the selection update (verified earlier, `:19232-19290`), which shows the item when the selection holds at least one qualifying song;
- one delegating branch in the action bar's `onItemClick`, beside `id == save_to` (verified earlier, `:3815`). If the item can own its click listener, this branch is not needed (checked at implementation).

**Shared-media multi-select** (`ui/Components/SharedMediaLayout.java`).
- One call after `deleteItem` is added (verified earlier, `:2262-2271`) adds a Purple item that owns its click listener, as upstream's items there do (verified earlier: `deleteItem.setOnClickListener(...)`, `:2271`).
- The Purple item shows itself only in the Music tab, and only when the selection holds a qualifying song.
- If showing it needs a selection callback, one more call goes where upstream refreshes the forward item (`updateForwardItem()`, `:2262`).

**Opening a share file.**
- One guarded early return in the `TYPE_FILE` branch of the message-tap handler, beside the existing Purple settings check (verified, `ChatActivity.java:41912-41916`): `if (PurplePlaylistShareImport.open(this, message)) return;`.
- A tap on a file that is not downloaded yet goes to the cell's download button first. `didPressButton` calls `FileLoader.loadFile` when `buttonState == 0` (verified, `ui/Cells/ChatMessageCell.java:17858-17870`). So on Android the first tap downloads the file and the next tap opens it, as with any file. **Open as playlist** in the message menu works at once.

**Player sheet** (`ui/Components/AudioPlayerAlert.java`).
- **One call in `buildSaveOptions`** (verified, `:2915`) adds **To Playlist** under **Save To**, as P8 asked. The Purple row checks `QualifySong` for the row's own message.
- **One call in `showMenuOptions`** (verified, `:2869`): `PurplePlaylistPicker.addSheetMenu(this, o, messageObject, noforwards || isMyList())`. It adds **Playlists**. When the last argument is true, it also adds **Add to playlist...** for the playing song, because upstream then hides Save To:
  - `addIf(!noforwards, ...)` hides it for a protected song (verified, `:2878`);
  - `isMyList()` skips it for the Profile music list (verified, `:2877`).

  `QualifySong` still decides, with the protected-content flag.
- **From the sheet, the picker opens with `sheet.show()`**, never through `showDialog`, which dismisses the dialog already on screen.

**Picker** (`ui/PurplePlaylistPicker.java`).
- A sheet listing the account's playlists, most recently used first, with check marks. It behaves as in section 4.3, with **New playlist**.
- For an album song, a "Whole album (N songs)" switch sits at the top.
- For a multi-select, a header reads "N songs".
- The confirmation bulletin offers Undo and **Open**.

**Top-level entry (P8 b, choice P25).** Android at HEAD has no side drawer. It uses bottom tabs (verified earlier, `MainTabsActivity.java:93-103`, `LaunchActivity.java:587`).
- The row goes in the chat list's main menu, `DialogsActivity.showItemOptions()` (verified earlier, `:13843`), right after the Saved Messages row (`:13950`).
- That menu has taken the drawer's place in this Android version, and already carries the Purple Work Mode row (verified earlier, `:13944`).
- The row is shown while `[playlists] menu_item_p` is true.

**Screens.**
- A **Playlists** row in `ui/PurpleSettingsActivity.java` opens `ui/PurplePlaylistsActivity.java`. It shows the list (recently used first), New, Recently deleted (with dates), the sync line and the note from section 6.1.
- That screen opens `ui/PurplePlaylistActivity.java`, which has:
  - Play, Shuffle and Download all;
  - rows, where a tap plays;
  - long-press for Remove, Show in chat and Use this copy;
  - drag handles using `ItemTouchHelper`;
  - Share, Rename and Delete in the top menu.

**Pinned music:** see section 5.4.

**Strings** go in the Purple block of `res/values/strings.xml`.

### 6.3 Naming next to upstream's Profile music

Unchanged, and P16 (a) stands.
- Both upstream clients already call Profile music a playlist: desktop's "Your playlist" (`lng_media_saved_music_your`) and Android's "Your Playlist" (`ProfilePlaylistTitleMine`).
- Their Save To menus label it "Profile".
- Purple's feature is called **Playlists**. The wording is "Add to playlist" and "To Playlist", and the Playlists screen carries the note.

## 7. Library format and merge rules

This is the part whose meaning must never change. It lives entirely in purple-core and is pinned by property tests.

### 7.1 Files

Each account has these files on each device:

- **Library.**
  - Desktop: `tdata/purple-playlists-<env>-<user id>.json` in the working directory, following `purple_last_seen.cpp:62-66` (verified earlier).
  - Android: `getFilesDirFixed()/purple/playlists/<env>-<user id>.json`, outside the sync store's root.

  The contents are the canonical payload bytes, uncompressed so they stay inspectable. The file is written atomically (`QSaveFile` or `AtomicFile`) and read back. It is the only source of what the next publish carries, so a crash can never leave an edit that is never published (section 8.5).
- **`.bak`.** Before any merge or import that changes the library, the previous file is copied to `.bak`, one level deep.
- **Local sync file**, `<library base>.local.json`. It holds:
  - the sync status: the last step, its result, and `unsynced_since`, the time of the first local change not yet in a present record;
  - the sync chat token the memo belongs to;
  - the memo;
  - `due_seen`: for each playlist due for the automatic purge, when a complete step first saw it due (section 7.9);
  - `hidden`: the pre-merge content of playlists that a merge deleted or purged, kept until the person dismisses the notice or uses Keep a copy (section 7.8);
  - the chats for which the person confirmed playlist sync despite other readers (section 8.10);
  - recent use (P19).

  Core parses this file and keeps unknown keys when it writes. It is never posted. Everything in it except `hidden` and the confirmations can be derived again. Losing it costs one extra download and, at worst, one extra publish.
- **Record cache**, `<library base>.records/`: the compressed bytes of each valid record in the memo, named by message id.
- **Share job and share history**, `<library base>.share-job.json` and `<library base>.shared.json` (section 10). Share history holds, for each destination chat, the document ids sent there and the copy map. It also holds each playlist's exchanged set.
- **Kept files**: on desktop the directory `<library base>.files/`, and on Android the list `<library base>.kept.json` (section 11).
- **Writers.** Each account has one serialized writer (desktop's main thread, or a single Android background queue), so a merge, an import and a local edit never interleave.
- **Logout** deletes all of these when the session ends (section 8.11).

### 7.2 Payload

```json
{
  "format": 1,
  "playlists": {
    "pl-k3f7q2abx7mnp4rt": {
      "deleted": [false, 1790000000123],
      "order": [["5361234567890123456"], 1790000000456],
      "songs": {
        "5361234567890123456": [
          {"alt": [{"msg": 88, "peer": "user:777000111"}],
           "meta": {"dur": 245, "perf": "Artist", "title": "Song"},
           "msg": 5521, "peer": "channel:1234567890",
           "since": 1790000000200, "un": "somechannel"},
          1790000000300],
        "5361234567890999999": [false, 1790000000900]
      },
      "title": ["Evening", 1790000000123]
    }
  },
  "purged": {"pl-zq5wd2ha6kcm3tve": 1}
}
```

- Document ids are unsigned decimal strings, because they can exceed 2^53. Android writes them with `Long.toUnsignedString`.
- The local file and the decompressed record are the same canonical form.

### 7.3 Validation (strict)

- **`format`** must be the integer 1. A larger integer is **NewerFormat**. Anything else is **Invalid**.
- **Keys.** Every object has only its listed keys.
  - In a playlist, `title`, `deleted`, `order` and `songs` are each optional, but at least one must be present. v1 always writes all four, but accepting partial playlists now lets a later version send deltas that v1 builds can still merge.
  - In a source value, `un` and `alt` are optional, and `alt` holds 1 to 3 entries when present.
  - Any unknown key makes the library Invalid.
- **Ranges:**
  - stamps and `since` are integers from 1 to 2^52;
  - `since` is at most its register's stamp;
  - `msg` runs from 1 to 2^31 - 1;
  - `dur` runs from 0 to 2^31 - 1.
- **Syntax:**
  - playlist ids match `pl-` plus 16 base32 characters;
  - document ids are 1 to 20 digits, with no leading zero, and at most 2^64 - 1;
  - peer tokens match section 3.1;
  - `un` is 4 to 32 characters of `[A-Za-z0-9_]`.
- **Text:** titles are 1 to 64 code points, and display strings are 0 to 96.
- **Consistency:**
  - order lists have no duplicates. They may name ids absent from `songs`, and display ignores such ids (section 7.8);
  - `alt` entries differ from the primary and from each other;
  - no purged id appears in `playlists`;
  - `purged` values are `1`.
- **Size** applies to input from the network only: records and share files (section 7.4). A local library file has no size cap.

Strictness is what makes pruning safe: a client never merges, and never prunes, a record it does not fully understand. Any future addition must bump `format`.

### 7.4 Encoding and the expansion guard

- **Encode:** core compresses the canonical bytes as one gzip member with zlib at level 6, with the header's mtime set to 0 and its OS byte set to 255. The output is deterministic for one core version, but nothing relies on that.
- **Decode(bytes, cap):**
  - require the gzip magic;
  - inflate as a stream (zlib `inflateInit2` with a window of 16 + 15 bits);
  - count the output, and abort with **Oversized** the moment it would pass the cap. Memory never goes beyond the cap plus one chunk;
  - require exactly one member, with no trailing bytes, and a matching CRC and length. Anything else is **Invalid**.
- **Caps (choice P29):**
  - **records:** at most **10 MiB compressed**, checked against the document size in the search result before downloading, and at most **32 MiB decompressed**. A record over either cap is Oversized and is never kept;
  - **share files:** at most 4 MiB compressed and 16 MiB decompressed;
  - **local edits:** refused past **24 MiB** (section 4.4);
  - **the local file:** no cap. A library that merges past 32 MiB still loads after a restart. It simply cannot be published until it shrinks (section 8.8).
- **Why these numbers.**
  - **10 MiB compressed** is desktop's limit for a download held in memory. `FileLoader` asserts, in release builds too, that a load with no target file is at most `kMaxFileInMemory` (verified, `storage/file_download.cpp:115`, and `storage/file_download.h:30`, where it is 10 MiB). The shipped reader downloads that way: `document->save(origin, QString())` (verified, `purple/purple_sync_candidate_reader.cpp:162-164`). Above 10 MiB, desktop would need a file-backed download and big-file upload parts, which is P29's option (b). The desktop refactor plan reaches the same cap for its Purple-owned upload (docs, refactor-plan-desktop.md, D3).
  - **32 MiB decompressed** gzips to about 9 MiB at the measured ratios, which fits.
  - **24 MiB for local edits** leaves room for 8 MiB of other devices' distinct data before a merged library outgrows what a snapshot can carry. Revision 2 claimed that two nearly full libraries still merge within the read cap. That is true only when they overlap heavily, and the claim is withdrawn.
  - Realistic libraries are around 1 MiB (7,200 songs), so these caps are guards, not targets. H19 measures time and memory at both sizes.
- **Where gzip lives.** In core, so both clients share one guard and one set of tests. The new file is `purple/purple_playlists_codec.{h,cpp}`. Core gains a zlib dependency:
  - desktop already links `desktop-app::external_zlib` (verified earlier, `Telegram/CMakeLists.txt:89`);
  - Android's `purplecore` links only `qt6core` and `log` (verified earlier, `TMessagesProj/jni/CMakeLists.txt:292-294`), so its list gains the NDK's `z`, which `tmessages` already links (verified earlier, `:222`);
  - core's `tests/run.sh` gains `-lz`.

### 7.5 A local file this build cannot read

- **Missing:** the library is empty (first use).
- **NewerFormat:** the Playlists screen shows only "These playlists were saved by a newer Purple. Update Purple to open them." Edits, merges, imports and sync are disabled, and the file is never written.
- **Invalid** (corruption, or a hand edit): the screen shows "Your playlists file can't be read", with **Start over**.
  - Start over renames the file to `<library file>.unreadable-<yyyymmdd-hhmmss>`, starts from an empty library, and clears the memo and the record cache.
  - The next step then restores whatever the sync chat holds.
  - Nothing else ever writes over an unreadable file.

### 7.6 Stamps

- **How a stamp is chosen.** An operation on playlist P takes s = max(now, the largest stamp or `since` anywhere in P, plus 1). Create uses now.
- **Where "now" comes from.** It is server-adjusted milliseconds where available.
  - Desktop computes `base::unixtime::now() * 1000 + QDateTime::currentMSecsSinceEpoch() % 1000`, as `InitializeSyncAccountLocally` already does (verified earlier, `purple_sync_account_setup.cpp:93-100`).
  - Android uses `ConnectionsManager.getCurrentTimeMillis()`, a native call (verified, `tgnet/ConnectionsManager.java:295-297`). Whether it includes the server time difference is H7.

  Correctness does not depend on the clock, except for the automatic purge date (section 7.9).
- **Why it works.** P's largest stamp already covers everything merged or imported into P. So a write made after seeing another write in P always wins.
- **Why per playlist.** A stamp far in the future, whether from a broken clock, a crafted record or a crafted share, affects only its own playlist. If P's stamps ever reach 2^52, edits to P are refused, but Delete forever still works.
- **Imports** copy the share's stamps, as a merge copies a record's. An import refuses any share with a stamp more than one day ahead of this device's clock (section 10.7).

### 7.7 Join and contains

- **Register:** keep the pair with the larger stamp. On equal stamps, keep the value with the larger canonical bytes. So on an exact tie:
  - a song beats `false`, so a re-add beats a removal;
  - `true` beats `false` for `deleted`, so a delete beats a restore.

  An absent register is the bottom element: join(absent, x) = x.
- **Playlist:** join `title`, `deleted` and `order` as registers. For `songs`, take the union by document id and join each register. The whole source value, fallbacks included, is one register value.
- **Library:**
  - `format` stays 1;
  - `purged` is the union;
  - `playlists` is the union by id, with each shared id joined, and then every purged id is dropped.

**Why join is safe.**
- Every piece is a maximum over a total order, taken key by key, with a bottom element for absent keys.
- Dropping purged ids commutes with the union, because `purged` only grows.
- So join is commutative, associative and idempotent over partial and full libraries alike, and it never creates a stamp.

**Why fallbacks are not merged as a set.**
- A union of fallback lists, cut to three, is associative on its own. But it breaks associativity against a removal: whether `false` wipes the fallbacks would then depend on the order in which three writes are joined.
- Keeping the whole value as one register loses one side's new source only when two devices relink the same song concurrently. That was already the rule for any concurrent relink.

**Contains(O, R)** is `canonical(join(O, R)) == canonical(O)`. A purge in O makes O contain an older R that still holds the purged playlist. That is how purged data leaves the cloud.

### 7.8 Display order and what each concurrent case gives

- **Visible playlists:** those that are not purged, whose `deleted` value is `false`, and that have both `title` and `deleted`. A playlist missing either register is **incomplete**: it is hidden, and cannot be edited until a record or an import supplies the rest.
- **Visible songs**, shown in two groups:
  - first, songs that are in the order list **and** whose `since` is at or before the order stamp, in list order. Order ids without a visible song are skipped;
  - then every other visible song, sorted by `since` and then by document id.

What follows from these rules:
- A song added after the last reorder, here or on another device, appears at the end.
- An album added in one step keeps album order.
- A song removed and later added again goes to the end.
- A relink, and an Undo of Remove, keep the song's place.

**Concurrent cases.** "Concurrent" means neither device had seen the other's write when it made its own.
- **Different songs added on two devices:** both stay, appended in `since` order.
- **The same song added on both:** one entry. If the two sources differ, one wins by stamp.
- **Renames on both:** one title everywhere, the one with the larger stamp. No notice is shown.
- **Reorders on both:** one whole order wins by stamp. Songs the other device added still appear, at the end.
- **A reorder on one device, and a relink or an Undo of Remove on the other:** both survive.
- **Remove on one device, and a relink or re-add on the other:** the larger stamp wins the song.
- **Delete on one device while the other adds songs:** the playlist is deleted everywhere, with the added songs inside it, hidden.
  - The adding device shows "“Gym” was deleted on another device. [Restore]".
  - Restore brings everything back everywhere.
- **Delete forever, or an automatic purge, on one device while the other edits the playlist:** the purge wins everywhere.
  - The editing device shows "“Gym” was deleted forever on another device. [Keep a copy]".
  - Keep a copy runs CopyPlaylist on the pre-merge playlist, taken from `hidden` in the local sync file. That survives restarts and later merges, unlike `.bak`, which the next changing merge overwrites.
  - Keep a copy is a button, not automatic. Dismissing the notice drops the `hidden` entry.
- **An automatic purge of a playlist that was already in Recently deleted on this device:** no notice, since nothing visible disappears.
- **Any edit made after seeing another write** always wins.

Removed songs, at about 43 bytes each, stay in the library until their playlist is purged.

### 7.9 The automatic purge (P3 d)

- **The rule.** A playlist is due when its winning `deleted` register is `[true, s]` and s + 30 days ≤ now.
- **Whose stamp.** Once devices have merged, the deletion stamp s is the same on every device, so every device computes the same date. Recently deleted shows it.
- **Where it runs.** Only inside a playlist step, on the **merged** library, after that step's merge (section 8.4, step 5). It never runs before the merge, and never directly from the Playlists screen.
  - Running before the merge let a device that had been away for weeks purge a playlist other devices had since restored. Its old copy still said `[true, s]`, and the step purged before searching.
- **Conditions, with sync on.** All four must hold:
  1. **Discovery was complete in this step:**
     - the search paged to its end;
     - every memo entry the search missed was confirmed present or gone by id;
     - every new candidate was read, or classified as invalid, newer-format or oversized, with no transient failure;
     - no memo entry was confirmed gone in a step that found no new valid record. A record that vanished without a new one appearing means another device pruned it after publishing a covering record this step cannot see yet.
  2. **No present record is newer-format.** A newer Purple may have restored the playlist in a record this build cannot read.
  3. **A request in this step succeeded** (the search). MTProto servers ignore messages whose `msg_id` is more than 300 seconds in the past or 30 seconds in the future (docs, core.telegram.org/mtproto/description). Both clients correct their clock when the server reports a bad `msg_id`: desktop through `correctUnixtimeWithBadLocal`, which forces the update (verified, `mtproto/session_private.cpp:2115-2118`). So after a successful request the clock is within about 5 minutes of the server's.
  4. **The playlist was already due at an earlier complete step at least 15 minutes before** (`due_seen`). An entry is cleared when a complete step finds the playlist no longer due. This absorbs search lag: a covering record published and indexed within 15 minutes is seen before anything is purged (H10).
- **Conditions, with sync off.** No other device can restore, so conditions 1, 2 and 4 do not apply. Condition 3 is met by one `help.getNearestDc` request, which Purple sends only when some playlist is due by the local clock, at the same times a step would run.
- **Why the old gate failed.** Revision 2 waited for desktop's `base::unixtime::updates()`. Verified: `update()` ignores a shift under 3 seconds, takes effect only once per process unless forced, and fires the stream only when the shift changes (`lib_base/base/unixtime.cpp:139-168`). On an NTP-synced Mac it never fires, so the purge would never have run. H23 is retired.
- **The largest-stamp guard is dropped.** A clock that is behind only delays a purge, so guarding against it prevented nothing.
- **Who performs it.** Any device may. The purge is the ordinary Delete forever. It takes no stamp, and a union of purged sets is idempotent, so two devices purging the same playlist agree.
- **What it publishes.** The purge changes the merged library, so the same step publishes a snapshot and prunes older records (section 8.5). The purged playlist leaves the cloud at that step.
- **Remaining risks, accepted:**
  - A restore made within about 15 minutes of another device's purge is concurrent with it. The purge wins, and the restoring device shows Keep a copy.
  - s is at least the playlist's last edit plus one (section 7.6). If the deleting device's clock was more than 30 days behind, *and* the playlist had not been edited for 30 days, other devices purge it at once. Server-adjusted stamps make this rare. Restore before the date prevents the purge.

### 7.10 Future formats

- **A format 1 build** never merges or prunes a record, and never imports a share file, whose `format` is not 1. It keeps publishing its own format 1 records, and shows "A newer Purple changed your playlists. Update to see those changes." While a newer-format record is present, it also skips the automatic purge (section 7.9).
- **Pruning in v1** happens only between two format 1 records.
- **A constraint on the format 2 design:** it must define an upgrade from format 1. Format 2 code may prune a format 1 record only when a format 2 record read in the same step contains that record's upgrade.

## 8. Sync

### 8.1 Why playlist records stand apart from settings records

Unchanged. A settings file cannot merge, so settings sync needs versions, spaces and ancestry. A library merges, so it needs none of them.

Playlist records therefore use a name and a caption that the shipped settings rule rejects.
- Verified: `IsSyncHistoryCandidate` (`purple_sync_inventory.cpp:64-78`, core `9c33c53`) accepts a non-forwarded document whose caption contains `#purplesync`, or whose name is exactly `Purple settings sync.json` or `Purple playlists sync.json`.
- `Purple playlists.json.gz`, with the caption `Purple playlists sync data #purpleplaylists`, matches neither. The caption's only `#` begins `#purpleplaylists`, which does not contain `#purplesync`.

`Purple playlists sync.json` stays reserved in core and is never posted. Nothing in shipped settings logic is edited.

### 8.2 The record, the sync chat, and sending

- **The record.** A document in the account's sync chat, sent directly rather than forwarded, with the `silent` flag so no member is notified.
  - File name `Purple playlists.json.gz`, MIME type `application/gzip`.
  - Caption `Purple playlists sync data #purpleplaylists`.
  - Bytes: gzip of the canonical library (section 7.4).
- **The candidate predicate.** Core exports the name, caption and tag, plus `IsPlaylistRecordCandidate(meta)`. It accepts a message that is:
  - regular;
  - not forwarded;
  - not sent via a bot;
  - sent by this account (`out`, H11);
  - carrying a document named exactly `Purple playlists.json.gz`.

  The file name is the record's identity, and the caption tag exists for search. The author check stops another member of a shared sync chat from injecting records.
- **How Purple sends** records, share files and protected copies. It uses a Purple-owned upload on each client, in Purple files:
  - `upload.saveFilePart` for files up to 10 MB, and `upload.saveBigFilePart` above that (docs, api/files: "in case the full size of the file is more than 10 MB");
  - then `messages.sendMedia` with its own random id and, for records and song copies, `silent`;
  - the returned Updates go to the client's own update handling: desktop `ApiWrap::applyUpdates(updates, randomId)` (verified, `apiwrap.h:159-161`), Android `MessagesController.processUpdates` (verified, `MessagesController.java:17949`).

  The new message, its id and its document id come straight from the result (H27).

  **Why not the existing send paths.** The shipped settings posts get their message id through receipt plumbing that Purple threaded through upstream send code:
  - desktop `ApiWrap::sendFiles` gained a `receipt` parameter (verified, `apiwrap.h:370`), added by commit `f7f536d165` along with edits to `apiwrap.cpp`, `storage/file_upload.{h,cpp}` and `storage/localimageloader.{h,cpp}` (verified with `git show --stat`: 12 files, +220 -52);
  - Android's `prepareSendingPurpleDocument(..., SendReceipt)` (verified, `messenger/SendMessagesHelper.java:9789`) hard-codes `notify` to true, so it cannot send silently (verified, `:9793`).

  The refactor must move or remove that code first: desktop D3 and Android C3 (docs, refactor plans). Its desktop plan recommends exactly this Purple-owned upload. Playlists therefore depend on no receipt plumbing. If the refactor builds the module for settings, playlists reuse it. Otherwise phases C and E build it.
- **What playlists need from the sync-chat layer** (sync-channel design):
  - the resolved sync chat of an account, or a status: none yet, waiting for permission to create the Purple channel, or unreachable;
  - **other readers**: none, members (with a count), or public (section 8.10 uses it);
  - a notice when the chat changes, carrying both the old and the new chat (section 8.13);
  - its kind, for three mechanical differences:
    - the search adds `from_id = self` in groups (H12);
    - deletes use `channels.deleteMessages` for channels and supergroups, and `messages.deleteMessages` with `revoke` otherwise (verified earlier, `api.tl:2466`, `:2766`);
    - posts to a broadcast channel go out as the channel;
  - **startup order.** Startup discovery (the marker search) runs before the playlist startup check. No new upstream hook is needed. The existing launch hooks, desktop `window_session_controller.cpp:1596` and Android `DialogsActivity.java:2874` (both verified earlier), already call into Purple, and the Purple side runs discovery and then the playlist check;
  - **warnings** about other members, and about someone new joining, belong to the sync-chat layer. The playlist confirmation (section 8.10) and the disclosure (section 9) refer to them;
  - **the chat-change confirmation**, owned by that layer, should say that playlist records move to the new chat and are deleted from the old one.
- **Readers.** The settings readers fetch a message by id and download its document. They re-apply the settings predicate, and are capped at 4 MiB (verified: desktop `purple_sync_candidate_reader.cpp:26`, `:116-131`, `:141-148`; Android `PurpleSyncReader.java:105-122`, verified earlier).
  - Each gains a playlist mode that takes the sync chat's peer, tests `IsPlaylistRecordCandidate`, caps the download at 10 MiB, and returns raw bytes. Both readers are Purple files.
  - On desktop the in-memory path stays safe below `kMaxFileInMemory` (section 7.4).
  - The desktop reader must read from the server, not from a Downloads copy. The review r2 finding G5 fixes that for settings, and the fix must cover this mode too.

### 8.3 Discovery

- **The search.** The step calls `messages.search` on the sync chat with:
  - `q = "#purpleplaylists"`;
  - `filter = inputMessagesFilterDocument`;
  - `from_id = self` in groups;
  - `limit = 100`.

  It pages by `offset_id` until a page comes back empty, or until it has seen `count` messages from the `messages.messagesSlice` or `messages.channelMessages` result. It does not stop at the first short page, because the server may return short pages before the end. The schema is verified earlier in `mtproto/scheme/api.tl:2463`. The step keeps the results that pass `IsPlaylistRecordCandidate`.
- **Precedent.** Purple already ships this request shape against Saved Messages: desktop's `SettingsSearchRequest` (verified earlier, `purple_sync.cpp:290-305`) and Android's `PurpleSyncOffer.search` (verified earlier, `:179-192`).
- **If the tag search fails (H10).** First fall back to querying the file name. If that also fails, send an empty query with the document filter, and page over every document in the sync chat. A dedicated channel makes that cheap.
- **Why search lag does no harm.**
  - A record the search missed is merged at a later step.
  - A missed record is never pruned, because pruning needs a covering record that was actually read.
  - A memo entry the search missed is confirmed by id before it is dropped (section 8.4, step 2).
  - The automatic purge waits out lag (section 7.9).

### 8.4 The playlist step

It runs:
- automatically (section 8.7);
- from **Sync now** on the Playlists screen;
- as the final step at logout (section 8.11);
- once against the old chat when the sync chat changes (section 8.13).

It never runs from the settings Check.

**Preconditions:**
- the library is readable (section 7.5);
- the effective `[playlists] sync_p` is on for the account, or the step is a manual Sync now or the final logout step;
- the sync-chat layer has a sync chat, and it is not public (unless P28 says otherwise);
- no step is already running for that account in this process.

**The steps:**

1. **Discover** (section 8.3). The result is a list of (message id, document id, document size, date) for the candidates.
2. **Compare with the memo.**
   - **New** candidates are those whose (message id, document id) pair is not in the memo. A record whose media was edited by hand has a new document id, so it counts as new.
   - **Missed** candidates are memo entries the search did not return. The step fetches them by id in one request: `messages.getMessages`, or `channels.getMessages` for a channel.
     - Those still there, and still passing the predicate, count as present.
     - Those gone lose their memo entry and cache file.
     - If that request fails, the step still continues, but discovery counts as incomplete.
   - Memo entries with a non-valid status whose classifier version differs from this build's are treated as new, so an upgrade re-reads them.
3. **Read the new ones.** Download each, up to 10 MiB, then decode it (up to 32 MiB) and validate it.
   - A valid record gets a memo entry with the hash of its canonical bytes, and its compressed bytes are cached.
   - An invalid, newer-format or oversized record gets a memo entry with that status and the classifier version.
   - A download that failed, or a candidate that vanished or was inaccessible, gets **no** memo entry and is tried again next step.
4. **Merge.** `merged` = the join of the local library and every new valid record. Records already in the memo were merged before, and the local library only grows, so they need no second join.
5. **Plan.** Call core `PlanPlaylistStep(local, merged, present records with status, hashes and cached bytes, discovery completeness, the request-succeeded flag, due_seen, now, sizes)`. It returns:
   - `purged`: the playlists the automatic purge removes now (section 7.9), applied to `merged`, and the new `due_seen`;
   - `writeLocal` and `hidden` (with pre-merge content), and a summary of added and removed songs;
   - `publish`: true when no present valid record has the canonical hash of `merged`, `merged` is not an empty library with no records present, and `merged` fits the record caps. Otherwise false, with a reason;
   - `prune`: the message ids of present valid records that another present valid record covers. The contents come from the cache. The checks run only when the present set changed, or when this step publishes;
   - counts of invalid, newer-format, oversized and unreadable records.
6. **Write locally.** If `writeLocal` is set, write the `.bak` and then the new file, save `hidden` into the local sync file, refresh the UI, and show one notice per hidden playlist.
7. **Publish.** Encode `merged` and send it through the Purple upload (section 8.2) into the sync chat, with `silent`. On success:
   - add the new record to the memo, with its hash and cached bytes, from the bytes in hand;
   - if the local library has not changed since this step began, clear `unsynced_since`.

   The just-published record then counts as read in this step. It contains every present valid record, because `merged` does, so core verifies containment for each one from the cache and prunes them now.
8. **Prune.** Delete each id in `prune`, with the method for the chat's kind. Failures are ignored and retried at the next step. Pruned ids leave the memo and the cache.
9. **Show status** (section 8.10).

**The steady state costs one request.** The search returns the records the memo holds, and the local library's hash equals one of them. So steps 3 to 8 do nothing: no download, no decompression, no parse, no join and no write.

**Is there a merge on every sync?** That was the owner's question under P15. In playlist-design.md, yes: every Sync now downloaded, parsed and joined every record, which automatic sync would make expensive. Now:
- a step that finds nothing new costs one search;
- a step that finds another device's new snapshot downloads it once, about 25 KB gzipped for 500 songs and about 350 KB for 7,200 (measured), and joins only that;
- the local file is rewritten only when the merge changed it.

### 8.5 When a step publishes, and why snapshots only

**The rule.** A step publishes a snapshot of the merged library whenever no present valid record already equals it. That single rule replaces the dirty set and the five compaction triggers of revision 2. It covers these cases:
- **An edit here.** The local library changed, so no record equals it.
- **A first publish, a changed sync chat, or a Start over followed by a merge that gained nothing.** No present record is this device's.
- **Two devices that published concurrently.** The next step on either one merges both, finds that neither equals the join, and publishes the join. That covers both, and they are pruned in the same step.
- **Records left by a failed prune, or one record covered by another.** No publish is needed, because some present record equals `merged`. The cover check prunes the others when the present set changes.
- **A purge.** The purge changes `merged`, so the step publishes and the purged playlist leaves the cloud.
- **A removal.** Its chat leaves the cloud at the next publish, within about 10 minutes under automatic sync. Revision 2 needed up to 7 days and choice R1.

**Crash safety.** What gets published is decided from the library file alone, which is written atomically. A crash at any point leaves at worst one extra publish or one extra download, never an edit that no record will carry.

**Why snapshots only in v1** (choice P30):
- the owner asked about performance and whether every sync merges (P15). The memo answers that without deltas: the steady state costs one search, and each device downloads each snapshot once.
- A snapshot of a realistic library is small: about 25 KB gzipped for 500 songs, 98 KB for 2,000 and 350 KB for 7,200 (measured). Automatic sync publishes at most once per burst of edits, which is at most once per 60 s quiet period and in practice a few times a day.
- Deltas needed dirty-key bookkeeping, five triggers with hourly limits, two acceptance tests and a product choice (R1), and they kept removed songs' chats in the cloud for up to a week.
- Format 1 still accepts partial playlists, so a later version can add deltas without a format bump if H19 or real use shows that snapshot traffic matters.

### 8.6 The cover rule, and why it cannot lose data

A device deletes record R only when it has read a record O that covers R, and both are valid format 1 records. "Read" means one of three things:
- downloaded in this step;
- taken from the cache under the same message id and document id that the search, or the fetch by id, returned in this step;
- published by this device in this step.

**Why no data is lost.**
- Records are libraries, full or partial, and absent registers are the bottom element. Containment is therefore the order of one join-semilattice:
  - reflexive, since join(R, R) = R by idempotence;
  - antisymmetric, in canonical form;
  - transitive, since join is associative.
- Covering adds the message-id tie-break, so it is a strict order, and no two records cover each other.
- Take any data in a deleted record R. Some record O covered R. If O was deleted too, some record P covered O, and then P also covers R.
- Posted records are finite, so this chain ends at a record nothing covers. No device can delete it, because covering depends only on content and message id, which every device sees alike. That record holds the data.
- Every device's local library only grows by join, import and local operations, so it still holds everything that device ever published.
- Pruning never changes the join of the records present, because each pruned record lies below a remaining one. That is also why the memo stays correct across prunes.

**The memo does not weaken the rule.**
- A cached record is used only under the same message id and document id that this step's search or fetch returned.
- Records are never edited in v1, and an edited record gets a new document id and is read afresh.

**Two assumptions**, both stated in the docs: nobody edits a record's media by hand, and nobody deletes records by hand. A hand deletion loses only data that lived solely on a lost device. The next publish by any live device puts everything it holds back.

### 8.7 When steps run (P4)

The automatic behaviour runs while `[playlists] auto_sync_p` is true, which is the default:

- **After edits.** Every local operation or import restarts a **60 second quiet timer** for that account. The step runs when the timer fires, or **10 minutes** after `unsynced_since`, whichever comes first. If a step is already running, one more is queued.
- **Checks.** At app start (after sync-chat discovery) and when the Playlists screen opens, a step runs if the last step for that account is at least **15 minutes** old. The interval is fixed. the owner asked for "at most every 15 min", and only automatic sync itself to be configurable.
- **Failures.** An automatic step that failed, for example offline, retries after 1, 2, 5 and 10 minutes, and then every 10 minutes while the library differs from every present record.
- **Manual.** **Sync now** always runs a step, whatever the options say.
- **Limits, stated.** Timers run while the process lives. On desktop, quitting before the timer fires leaves edits waiting until the next start. On Android, a killed process does the same. The library is on disk, so nothing is lost, only delayed.

With `auto_sync_p` false, only **Sync now** and the final logout step run.

### 8.8 Failure cases

- **Invalid record:** ignored and never pruned. The status counts it: "1 playlist sync message can't be read". The person can delete it by hand.
- **Newer-format record:** ignored and never pruned. The status asks for an update, and the automatic purge waits.
- **Oversized record** (over 10 MiB compressed, or expanding past 32 MiB): never downloaded or kept, and never pruned. The status counts it.
- **A candidate that vanished, failed or was inaccessible:** skipped this time, not added to the memo, and discovery counts as incomplete.
- **Post failed:** the status says so, and the next step tries again, because the library still differs from every record.
- **Delete failed:** retried at the next step.
- **Merged library too large to publish** (over 32 MiB, or over 10 MiB compressed): reading and merging continue, but this device publishes nothing. The status says "Playlists are too large to sync from this device. Delete forever deleted playlists, or remove songs." Local edits are refused above 24 MiB.
- **No sync chat yet:** the status says "Playlist sync needs a sync chat · Set up", which opens the sync-chat layer's setup.
- **Public sync chat** (with P28's default): "Playlist sync is paused: <chat> is public. · Choose another chat".
- **Unconfirmed chat with other readers:** "Playlist sync is waiting for your confirmation · Review" (section 8.10).
- **Switch off:** nothing is searched, sent or deleted. Existing records stay in the sync chat until the person or the sync-chat layer removes them.

### 8.9 Options

All options live in settings.toml under `[playlists]`. They are read through the account-settings design's per-account overrides (P10). Toggles on the Playlists screen write the active account's override, with the same surgical splice as the Premium toggle, which appends the table when it is absent (docs, `docs/purple/config.md:34-47`).

- `sync_p` (default true): P21, subject to section 8.10.
- `auto_sync_p` (default true): the automatic behaviour of section 8.7 (P4).
- `menu_item_p` (default true): the top-level Playlists row (P8).
- `keep_downloaded_p` (default true): section 11 (P6).

Protected content (P13) and private listening (P20) are user-control keys, named by that design.

**Saying where a toggle applies.** settings.toml is itself carried by settings sync, so a toggle reaches every device that syncs settings.
- Until per-account overrides exist, the global keys apply to every account. The toggle, and the first-run bulletin's **Turn off**, then say "Turns playlist sync off for all accounts on every device that syncs settings."
- With overrides, they say "...for <account> on every device that syncs settings."
- A question for the account-settings design: if its sections are keyed by user id, a toggle made on account B writes B's id into the file that settings sync posts to account A's sync chat. Either key sections by something else, or name that link in its disclosure. This design does not decide it.

### 8.10 The sync line, the first-run bulletin, and chats with other readers

The Playlists screen on each client has one sync line. It shows one of:
- "Syncing...";
- "Synced 5 min ago · Sync now";
- "Changes waiting to sync · Sync now", while `unsynced_since` is set. The Add toast repeats it as a second line only when automatic sync is off;
- "Offline: changes will sync when you're connected";
- "Sync is off · Turn on";
- the problems in section 8.8, and "A newer Purple changed your playlists. Update to see those changes."

**The first-run bulletin** replaces the old confirm-to-enable dialog, since sync is on by default (P21).
- It appears where the account's first playlist is created: in the picker, as a bulletin, or at the top of the Playlists screen.
- It reads "Your playlists sync through <sync chat> so your other devices get them. Learn more · Turn off". With no sync chat yet, it reads "Playlists will sync once you set up a sync chat. Set up".
- **Learn more** shows the disclosure (section 9).

**Chats with other readers (P28).** the owner consented to P21's default-on sync when the destination was private Saved Messages. Records name the people, groups and private channels that songs came from, so posting them where others can read them needs its own consent.
- **No other readers** (Saved Messages, or a Purple channel with no other members): default on, as P21 chose.
- **Members.** Before the first playlist publish into that chat, a dialog appears: "Sync playlists through <chat>? <N> other members of <chat> will be able to read your playlist names, song titles, and the ids of the people, groups and channels your songs came from." The buttons are **Sync playlists** and **Keep on this device**.
  - Keep on this device turns `sync_p` off for the account.
  - Until the person answers, steps still search and merge, but publish nothing.
  - The answer is stored per chat in the local sync file, so each device asks once for each chat.
- **Public** (by default): playlist records are refused, with the status line from section 8.8. `channels.searchPosts` would let anyone find them by hashtag (docs, api/search).
- **Someone new joins later:** the sync-chat layer warns. Choosing another chat moves the records (section 8.13).

### 8.11 Logout (P18)

**The mechanism has two parts.**
- **A hold** before the logout starts, which runs a final step or shows a dialog.
- **A deletion** when the session actually ends, observed from Purple code:
  - desktop: an observer on `account->sessionChanges()` for each account (verified public, `main/main_account.h:79`) sees `nullptr` while `!Core::Quitting()` (verified, `core/application.cpp:2017-2019`). The observer remembers each account's user id while the session is live. The session is also destroyed on quit (verified, `main/main_account.cpp:60`), which is why the quitting check is needed;
  - Android: an observer of `NotificationCenter.appDidLogout` for each account, which `performLogout` posts for every logout type (verified, `MessagesController.java:16247`). `performLogout` clears the user config before posting it (verified, `:16231`), so the observer uses the user id it recorded while the account was live.

  Both observers are registered from Purple's existing startup calls, so no upstream hook is needed. H26 checks that they fire for user, forced and forgotten-passcode logouts, and never on quit.

**Why deletion waits for the session to end.** On desktop, `logoutWithChecks` goes on to ask about running exports, uploads and downloads, and each of those prompts can cancel the logout (verified, `core/application.cpp:979-999`). A hook that deleted files before them could leave a signed-in account with its playlists gone.

**With sync on:**
- The hold runs a final step with the progress line "Syncing playlists before logging out". If the library already equals a present record in the memo, it skips the step, so an offline device can still log out without a prompt.
- On success, the hold calls upstream's `retry`. The second pass consumes a one-shot flag and returns false, so upstream's own checks then run. If one of them cancels, the flag is already gone and the files are untouched. The next logout attempt runs the (cheap) final step again.
- If the step fails with changes waiting: "Couldn't sync your playlists. Log out anyway and delete them from this device? Changes since <time> will be lost." with **Try again**, **Log out anyway** and **Cancel**.
- **With no sync chat, or with publishing held for confirmation:** "Playlists of <account> aren't synced yet. Set up sync, or log out and delete them from this device." with **Set up sync** (or **Review**), **Log out and delete** and **Cancel**. Try again could not succeed there.

**With sync off:**
- The hold warns: "Playlists of <account> aren't synced. Logging out deletes them from this device."
- The buttons are **Sync once, then log out** (one step to the sync chat whatever the switch says, offered only when a usable sync chat exists), **Log out and delete** and **Cancel**.
- With no playlists, nothing is shown.

**A forced logout** (the session ended elsewhere) cannot sync. By default the session-end observer deletes the files, as upstream wipes the account's local data (verified: `Account::forcedLogOut` calls `loggedOut()`, which calls `local().reset()`, `main/main_account.cpp:539-550`). A remote session end is often a lost or stolen device, and with automatic sync the unsynced window is normally under 10 minutes. The docs state that edits from those last minutes can be lost. Choice P27 keeps the alternative.

**Hooks:**
- Desktop: one guarded early return at the top of `Core::Application::logoutWithChecks` (verified, `core/application.cpp:979`): `if (account && account->sessionExists() && Purple::PlaylistsHoldLogout(account, retry)) { return; }`.
- Android: one guarded early return at the top of `MessagesController.performLogout(int type)` for `type == 1`, the user-initiated logout that `LogoutActivity` calls (verified, `MessagesController.java:16215`; verified earlier, `LogoutActivity.java:167`). Nothing after it can cancel the logout (verified, `:16215-16264`).

### 8.12 What other builds see

- **Shipped Purple builds** (desktop `3ac486a86d`, Android `dca9a0393`) ignore playlist records. Verified: the settings rule (section 8.1), and the launch-time offer, which looks only for `settings.toml` documents in Saved Messages (verified earlier: `PurpleSyncOffer.java:94-104`, `purple_sync.cpp:290`).
- **Auto-download.**
  - Shipped Purple Android excludes only the two reserved names (verified earlier, `PurpleSyncAutoDownload.java:28-31`). New builds add `Purple playlists.json.gz`, and also `*.purpleplaylist` for tidiness.
  - The desktop G1 and G2 fixes planned for tonight must cover both names. Otherwise opening the sync chat drops a record copy into `~/Downloads`. The Purple upload removes G1's post-side copy for playlist sends.
  - Official clients may auto-download records when the sync chat is opened (H8).

### 8.13 Changing the sync chat

When the sync-chat layer reports a change from chat A to chat B:
1. Run one step against A, if A is still reachable: discover, read and merge. Nothing is published there.
2. Run a normal step against B. B has no memo yet, so the step publishes a snapshot into B (section 8.5).
3. Delete from A each valid format 1 record by this account that B's new snapshot contains, checked in core from the cached bytes of step 1, with A's delete method. Invalid and newer-format records are left, and the status says "2 playlist sync messages were left in <A>".
4. Only then clear A's memo and cache.

- If A is unreachable (left, deleted, or no rights), steps 1 and 3 are skipped, and the status says that old records may remain there.
- Another device of the account keeps using A until settings sync tells it about B. When it switches, it runs the same procedure, so its later records in A are merged and removed.
- Revision 2 cleared the memo at once and never touched A again. That threw away the ids needed for cleanup, and left DM partner ids readable by whoever made the person switch.

## 9. Privacy: what leaves the device

**Disclosure**, behind the first-run bulletin's **Learn more** and on the Playlists screen:

> Playlists of <account> sync through <sync chat>. Purple posts a file named "Purple playlists.json.gz" there. Telegram's servers, every device signed in to <account>, and <if the chat has other readers: the <N> other members of <sync chat>> can read it. It is not end-to-end encrypted.
>
> It lists your playlist names and, for each song, its title, performer and length, and which chat and message it came from, including earlier copies you replaced. That includes numeric ids of people, groups and private channels, and a channel's public username. It also records when each change was made, to the millisecond.
>
> Deleted playlists stay in it until you use Delete forever, or until 30 days in Recently deleted have passed. Removed songs leave only an id and a time behind, and their chats leave the cloud at the next sync. Purple deletes older playlist sync messages once a newer one contains everything in them.
>
> Audio files, your play queue, your settings and other accounts' playlists stay on this device. Songs you keep downloaded are stored as plain files on this device. Sharing a playlist sends the songs from you, and a separate playlist file, to the chat you pick. That file never says where the songs came from, but it shows when each song was added. <If private listening is off: Playing a song reports listening to Telegram, as the official apps do.>

**Precisely, a record contains:**
- playlist ids and titles, including deleted playlists until they are purged, and the ids of purged playlists;
- for each member song: the document id, the peer type and id, the message id, `un` for public channels, up to three earlier sources, the title, performer, duration and `since`;
- for each removed song: the document id and a stamp;
- order lists;
- stamps.

**Never in a record:**
- audio;
- access hashes and file references;
- the account's own user id (`self` is used instead);
- local paths;
- availability and download state;
- the queue and playback positions;
- recent use;
- share history, exchanged sets and copy maps;
- anything from `settings.toml` or `state.toml`;
- any device name, platform, app version or install id.

Acceptance X6 scans every posted record for these.

**Local only:** the library, its `.bak`, the local sync file, the record cache, the share job and share history, and kept files. Kept audio on desktop sits outside the media cache, which is encrypted with the account's local key (verified, `storage/storage_account.cpp:1831-1838`). The kept files are plain (section 11).

**Logs.** Playlist code logs counts, statuses and error types only. It never logs titles, peer tokens, usernames, document ids, destinations or sender names. That matches the existing sync code, which logs only the error type (verified, `purple/purple_sync.cpp:326-328`). X6 also scans the logs.

With `sync_p` off, nothing playlist-related leaves the device, except shares the person sends on purpose.

## 10. Sharing

### 10.1 What it does

Share sends a playlist to a chat:
- **every playable song**, forwarded with `drop_author` and `drop_media_captions`. Recipients see plain audio messages **from the sharer**, with no "Forwarded from" header and no caption. This works for recipients on any Telegram client;
- then **a share file** that a Purple recipient opens as a playlist, or merges into an earlier import of the same playlist after a preview.

Songs from protected chats cannot be forwarded. They are re-uploaded from this device after a confirmation, once the user-control setting exists (section 10.5).

### 10.2 Identity: the playlist id itself

**Decision:** the share file carries the playlist's own id, and the recipient's import keeps that id. There is no separate share id.

**Why.**
- **Concurrent imports converge by themselves.** A recipient who imports the same share on two devices before they sync gets two writes under one id, and the join merges them. With a separate share id, each device would mint its own internal id, and syncing would leave two playlists carrying the same share id.
- **Re-shares merge with no mapping table.** When the recipient shares the playlist back, or onward, the file carries the same id, so every holder merges it into the same playlist.
- **No new format field.**

**The costs, and how each is handled.**
- **Re-keying loses the link.** Keep a copy, and Import as a new playlist, create a new id. A re-share of that copy is a new playlist for recipients.
- **A purged id.** An id purged in the recipient's library, or in the sharer's, cannot merge. The import offers "Import as a new playlist" under a fresh id.
- **The id is revealed to recipients**, and it links every share of that playlist, which is the point of the feature.
  - It is 80 random bits and says nothing about content.
  - Anyone holding it could craft a file aimed at that playlist, but every import is shown before it applies (section 10.7), and stamps more than a day ahead are refused. The effect of a hostile file stays inside one playlist and can be undone.

### 10.3 The sender's flow (both clients)

1. **Entry:** **Share...** in the playlist's menu, and in the row menu on the Playlists screen.
2. **Choose a chat, or a forum topic.**
   - Desktop uses `Window::ShowChooseRecipientBox` (verified earlier, `window/window_peer_menu.h:203-208`). Its `chosen` callback accepts only a thread where the account may send music and files (verified earlier, `ChatRestriction::SendMusic` and `SendFiles`, `data/data_chat_participant_status.h:59`, `:61`).
   - Android uses `DialogsActivity` in select mode with `DIALOGS_TYPE_FORWARD` (verified earlier, `:1366`, `:3509`), with the same rights checks (verified earlier, `ChatObject.canSendMusic`, `canSendDocument`, `ChatObject.java:2188`, `:2195`).
   - **Refused, with a one-line reason:**
     - a group with slow mode, unless the account is an admin. Docs: forwarding several messages there fails with `SLOWMODE_MULTI_MSGS_DISABLED`;
     - **a chat that charges for messages**. Desktop checks `starsPerMessageChecked() > 0` (verified, `data/data_peer.h:338`), and Android checks `MessagesController.getSendPaidMessagesStars(dialogId) > 0` (verified, `:7316`). `messages.forwardMessages` takes `allow_paid_stars` (verified, `api.tl:2471`), and upstream handles `ALLOW_PAYMENT_REQUIRED` in its own share and send paths (verified, `boxes/share_box.cpp:1971`, `apiwrap.cpp:525`). Purple's requests skip that handling, so they would fail batch after batch. Nothing in the decisions asks for paying per song.
3. **Resolve** every visible song in display order, with fallbacks, and take a snapshot of the playlist at that moment. The file describes that snapshot.
4. **Classify** each song:
   - **forwardable;**
   - **protected:** on desktop `HistoryItem::forbidsForward()` (verified earlier, `history_item.h:480`); on Android the message's `noforwards`, or `MessagesController.isPeerNoForwards` (verified earlier, `:6782`);
   - **unavailable.**
5. **Confirm.** "Share “Evening” to <chat>?", followed by:
   - "Sends 42 songs as 42 messages from you, without “Forwarded from” headers or captions, then a small playlist file that Purple Telegram can open. The file also shows when each song was added."
   - "3 songs are unavailable and will be left out", when any are.
   - For a chat this device shared this playlist to before (from share history), a choice between **Send only the 5 new songs** (the default, choice P23) and **Send all 42 again**.
   - Above 100 songs: "Telegram may slow large shares down. This can take several minutes; keep Purple open until it finishes."

   The buttons are **Share** and **Cancel**.
6. **Protected songs** (section 10.5).
7. **Run the share job** (section 10.4).
8. **Send the share file** after every song has gone (section 10.6). Only then are the protected copies' document ids known, and only then do the songs sit before the file in the chat, where the recipient's search expects them.
9. **Done:** "Shared “Evening” to <chat>: 42 songs", plus "3 left out" when any were. Share history records the destination, the document ids sent, and the copy map. The playlist's exchanged set gains the sent ids.

### 10.4 Forwarding mechanics

- **Batches.**
  - A batch is a run of consecutive songs, in playlist order, from one source chat, with at most 100 ids per `messages.forwardMessages` request.
  - Verified earlier: Android's own forward path cuts batches at 100, and at every change of source chat (`SendMessagesHelper.java:2550`). It sets `drop_author` and `drop_media_captions` from the "hide sender" and "hide caption" options (`:2583-2584`).
  - Verified earlier: desktop's `ApiWrap::forwardMessages` sends one request per run of items from one source, and sets `f_drop_author` and `f_drop_media_captions` the same way (`apiwrap.cpp:3793-4030`, flags at `:3872-3877`). It does not cut at 100, and it does not report the new ids.
  - Docs: the method page names `drop_author` ("Whether to forward messages without quoting the original author") and `drop_media_captions` ("Whether to strip captions from media"), but states no id limit. The 100 comes from the clients (H14).
- **Purple sends its own requests** from a Purple file on each client, with its own random ids. It passes the resulting updates to the client's update handling, as upstream does after its own sends (section 8.2). This needs no upstream edit, and gives Purple control of batching, pacing, random ids and errors.
- **Order.** Batches go strictly one at a time, each after the previous response. Within a batch, ids go in playlist order. If the server orders them by id instead, non-Purple recipients see chat order within that batch (H14). The share file carries the true order either way.
- **Notifications.** Song batches are sent `silent`. The share file is sent normally, so the recipient gets one notification instead of hundreds.
- **Flood waits.**
  - On `FLOOD_WAIT_X`, and on `FLOOD_PREMIUM_WAIT_X` (docs, api/errors: "A wait of X seconds is required"), the job waits X seconds and shows "Telegram asked Purple to wait 38 s". It then retries the same batch with the **same random ids**.
  - A batch whose response was lost is retried the same way. The server's duplicate detection should then create no second copy (H17), and a duplicate-id error counts as sent.
- **Errors.**
  - `CHAT_FORWARDS_RESTRICTED` (docs: "You can't forward messages from a protected chat") on a batch the check thought forwardable means protection changed in the meantime. Those songs move to the protected path.
  - An error for one batch marks its songs as failed, and the job continues.
  - Losing the right to write to the destination, or a paid-message error, stops the job.
- **Persistence.** The job is saved in `.share-job.json` after every batch: the destination, the snapshot, per-song state, per-batch random ids, and the copies made.
  - After an interruption, the next start shows "Sharing “Evening” to <chat> stopped at 120 of 420. [Resume] [Discard]".
  - **Cancel** stops after the current request and discards the job, and no file is sent.
  - Each account has at most one job at a time.

### 10.5 Songs from protected chats

**The dialog.** It is shown when the share includes protected songs and `protectedContentAllowed` is true:

> **7 songs are from protected chats**
>
> Telegram doesn't allow forwarding them. Purple can upload copies from this device instead. This downloads 18.4 MB for songs not yet on this device and uploads 41.2 MB. The copies will not be protected: anyone in <chat> can save and forward them.
>
> [Upload copies] [Skip these songs] [Cancel]

- The sizes come from the documents' sizes. The download figure counts only songs without a complete local file.
- **Skip these songs** leaves them out of the forwards and out of the file.
- **With the flag false**, which is the state until the user-control layer exists, there is no dialog. The confirmation says "7 songs from protected chats will be left out." Once the setting exists, it adds "<setting name> in Purple settings lets Purple upload copies instead."

**Uploading a copy**, at the song's place in order:
- **Getting the bytes, without moving the document's recorded location** (1.17):
  - desktop reads, in order: the kept file (section 11); the document's own local file, if `location(true)` finds one readable; the in-memory bytes. It never calls `DocumentData::save` to a temporary path. That call sets the document's location to the path whenever the bytes are in memory (verified, `data/data_document.cpp:1202-1214`), and deleting the temporary file afterwards would break a kept song's offline playback;
  - if no local copy exists, desktop downloads the song into the kept directory, as Download all does. If Keep downloaded is off, that file is deleted after the upload. A missing file only makes the next play stream, because `location(true)` re-checks (verified, `data/data_document.cpp:1414-1425`, `core/file_location.cpp:78-89`);
  - Android uses the downloaded media file, downloading it first if needed.
- **Sending** goes through the Purple upload (section 8.2) as an audio document with the song's audio attributes, with `silent` set, using big-file parts when it is over 10 MB. The new message's document id is recorded as the copy's id.
- The copy carries the file's own tags. Where the original's Telegram attributes differ from its tags, the copy shows the tags (H18).

**Why a copy is needed.** Docs, api/files: "Non-protected media (i.e. media from messages without the noforwards flag) may be resent to other chats, without reuploading it". A protected song therefore has no server-side shortcut.

**Identity and the copy map.**
- In the share file, a copied song is keyed by the **copy's** document id, with the original's display fields, `since` and stamp. The sharer's own library still points at the original.
- The copy map lives in share history on the device that made the copies.
  - **When building a file**, that device substitutes copies in `songs`, in `order` and in tombstones. A later removal of the original therefore reaches recipients as a removal of the copy.
  - **When importing a file**, that device maps each copy id back to its original before merging. For a mapped song, only a tombstone is adopted. A non-`false` value never overwrites the local original, whose source is the real one. So a playlist sent to a friend and shared back does not gain duplicate entries on the device that made the copies.
- **A re-share from the same device** reuses an earlier copy already in that chat.
- **Known limit.** The copy map is not synced. A re-share from another device of the sharer uploads new copies, and an import on another device of the sharer adds the copy beside the original. The person removes the duplicate by hand. Syncing the map would need a new field in the source value, for a rare case.

### 10.6 The share file

- **Name:** `<title>.purpleplaylist`.
  - The title is sanitized: characters other than letters, digits, spaces and `-_().` become `_`, which includes `#`.
  - It is cut to 60 code points, and becomes "Playlist" if empty.
  - MIME type `application/gzip`.
- **Caption:** "Playlist file with 42 songs. Open it in Purple Telegram to save the songs above as a playlist."
  - It carries **no title and no `#`**.
  - A title such as "#purplesync mix" in the caption would make the file a settings-sync candidate on every build: `IsSyncHistoryCandidate` accepts any non-forwarded document whose caption contains `#purplesync` (verified, core `9c33c53`, `purple_sync_inventory.cpp:64-70`). If it were shared into the home account's Saved Messages, it would read as Invalid, and `AggregateSyncCandidateRead` would return NeedsReview (verified, `:118-132`), stalling settings sync.
  - The file name already shows the title.
- **Content:** gzip of canonical JSON, at most 4 MiB compressed and 16 MiB decompressed, under the same guard as records:

```json
{
  "format": 1,
  "kind": "share",
  "order": [["5361234567890123456", "5361234567890777777"], 1790000000456],
  "playlist": "pl-k3f7q2abx7mnp4rt",
  "songs": {
    "5361234567890123456": [{"meta": {"dur": 245, "perf": "Artist", "title": "Song"}, "since": 1790000000200}, 1790000000300],
    "5361234567890777777": [{"meta": {"dur": 198, "perf": "Other", "title": "Copy"}, "since": 1790000000210}, 1790000000210],
    "5361234567890999999": [false, 1790000000900]
  },
  "title": ["Evening", 1790000000123]
}
```

- **The file carries:**
  - the playlist id, title register and order register. The order has copies substituted and left-out songs removed;
  - each shared song's display fields, `since` and stamp;
  - **a tombstone** (document id and stamp) for each removed song **whose document id is in this playlist's exchanged set** on this device, with copies substituted. Removals propagate to recipients who could have the song. Songs that never left this device reveal nothing.
- **It never carries:**
  - `peer`, `msg`, `un` or `alt`;
  - the deleted flag or the purged set;
  - other playlists;
  - the sharer's user id;
  - device data.

  So it cannot name the sharer's source chats or message ids. C22 is a property test that no such field can appear.
- **What it does reveal, and the disclosures say so:** when each song was added and last changed, to the millisecond, because merging depends on stamps; and the playlist id, which links shares of one playlist.
- **Why it holds no destination message ids either.** In private chats and basic groups, each side numbers messages separately (section 3.1), so the ids the sharer sees are not the ids the recipient sees. Recipients find the songs by document id instead, which forwarding keeps (section 10.7). Uploaded copies get their document ids before the file is built.
- **Limits:** at most 5,000 songs per share. A larger playlist offers to share its first 5,000 songs in display order.

### 10.7 The recipient's import (both clients)

**Opening.**
- Tap the file: the desktop hooks (section 6.1) or the Android tap hook (section 6.2).
- Or use **Open as playlist** from the message menu, through the existing menu hooks.
- Purple downloads the file in memory (at most 4 MiB), decodes it (at most 16 MiB) and validates it.

**Finding the songs.** Only the songs the recipient needs are looked up: those not already in their copy with an equal or newer stamp. The lookup is `messages.search` in the chat that holds the file, with:
- an empty query;
- `inputMessagesFilterMusic` (verified earlier, schema `api.tl:309`);
- `from_id` = the file's sender, except in broadcast channels;
- `max_id` = the file's message id;
- `top_msg_id` for a forum topic.

It pages backward until every needed document id is found, or until 50 pages (5,000 messages) have been scanned. The newest match wins (H20). Songs not found are left out and counted.

**The preview.** Core `PlanShareImport(library, share, resolved, copy map, now)` decides which of these the recipient sees:
- **New:** "Import playlist “Evening” from <sender>: 42 songs (2 not found in this chat)", with **Import** and **Cancel**.
- **Update**, when the id exists and is not purged: "Update “Evening” from <sender>", with a summary:
  - songs added and removed by the file;
  - a changed title or order, when the file's register is newer;
  - "Your own changes stay", listing what the recipient changed later.

  The buttons are **Merge**, **Import as a new playlist** and **Cancel**.
- **Purged here:** "You deleted this playlist forever. Import it as a new one?" with **Import as new** and **Cancel**.
- **Up to date:** "“Evening” is already up to date with this file", with **Open**.
- **The account's own share:** the file's sender is this account and the id exists here. "This is your shared copy of “Evening”", with **Open**. Nothing is merged, which avoids pulling copies in beside their originals on a device without the copy map.
- **Newer format, Invalid, or too large:** "This playlist file was made by a newer Purple", or "This playlist file can't be read".
- **A stamp more than one day ahead of this device's clock:** only **Import as a new playlist** is offered.

**Staying linked, and detaching.** Nothing merges without a tap and this preview, so a recipient who wants no updates simply taps Cancel, or Import as a new playlist, on later files. There is no switch and no Unlink. Revision 2's "Keep receiving updates" switch promised updates that never arrived by themselves, and Unlink needed an operation, a purge and a list of imported playlists.

**The merge (ImportShare).**
- On the device that made copies, copy ids are first mapped to their originals (section 10.5).
- For the title, the order, and each song and tombstone, the import adopts the file's register only when its stamp is **larger** than the local one, or the local one is absent. On an equal stamp the local value stays. That keeps the recipient's own sources for songs they already had, for example on the sharer's side when a re-share comes back.
- Adopted song values get `peer` = the chat where the file sits, as seen by the recipient, and `msg` = the message found.
- Imported stamps are copied, not created, so later edits on either side keep winning by the stamp rule.
- If the local playlist was in Recently deleted, the import restores it with a fresh stamp. If it did not exist, the import creates it with `deleted=[false, fresh]`.
- Every adopted document id joins the playlist's exchanged set.
- The import changes the library, so the recipient's own sync publishes it.
- **Undo** restores every register the import wrote.

**What does not depend on order.** Importing two files in either order gives the same visible songs, title and order register. The source a song points to can differ, because ties keep the local value and each file rewrites sources to its own chat. Test C23 checks exactly that.

**Both sides' edits survive, at register level.**
- A song the sharer added, or removed later, reaches the recipient.
- A song the recipient added, or removed later, stays.
- Title and order are whole registers, so the later write wins, whoever made it. The update preview says which side wins.

**The result:** "Imported “Evening”: 40 songs, 2 not found in this chat · Open". The new songs' sources are messages in that chat. If the recipient later loses access to the chat, those rows show No access and can be relinked like any other.

### 10.8 Re-sharing

- **The same device, the same chat:** section 10.3 offers "only the new songs" (the default) or "all again". The file always describes the whole playlist. Songs already in the recipient's copy with equal or newer stamps need no lookup, so the recipient searches only for the newly sent songs, which sit just before the file.
- **Another chat, or another device:** everything is sent, because there is no share history for that chat.
- **A recipient re-shares:** the file carries the same id, the recipient's register values and stamps, and sources rewritten to the new chat. Anyone holding the playlist merges it the same way, including the original sharer, whose device that made the copies maps them back.

### 10.9 What recipients on other clients see

- N audio messages from the sharer, with no "Forwarded from" header and no captions, in playlist order batch by batch (H14).
- Songs that were one album in the source and went in one batch may appear grouped as an album (H15).
- Protected songs appear as ordinary uploaded audio.
- Then one small file named "Evening.purpleplaylist", with the caption explaining that Purple Telegram can open it. Official clients show a generic file. Tapping it offers to save it, or finds no app, which is harmless.

### 10.10 Large playlists

- At most 5,000 songs per share, at most 100 per request, and one request at a time.
- Flood waits are honoured with a visible countdown, and the job persists across restarts.
- The confirmation warns above 100 songs.
- A 2,000-song share from few chats takes at least 20 requests, and more when the source chat changes often. H16 measures real rates.
- Recipients' lookups are bounded at 50 pages.

### 10.11 Limits stated in the docs

- A re-share from another device duplicates protected copies for recipients, and an import on another of the sharer's devices adds copies beside originals.
- A recipient who joined a group after an earlier share, where the group hides old history from new members, may not find the earlier songs. **Send all again** fixes it.
- Songs the recipient deleted from the chat before importing show as "not found".
- The order within a batch depends on the server (H14).
- A tombstone reaches recipients only from a device that exchanged that song.

## 11. Offline listening (P6 b and c)

**Download all**, in the playlist view, downloads every resolvable song of that playlist. It shows a progress row "Downloaded 120 of 140 · 2 failed · Retry" and a downloaded mark on each row.

**What Keep downloaded means, on both clients:** files fetched by **Download all** for a song of an unpurged playlist are exempt from Keep Media and the cache limits. A song that was merely played is not kept.

**Desktop.**
- With `keep_downloaded_p` on (the default), each song is saved into `<library base>.files/<document id>.<extension>` with `DocumentData::save(origin, path)`.
- **Making the kept file the document's source.** `save` does not always record the target as the document's location (verified, `data/data_document.cpp:1202-1225`):
  - with bytes in memory, it writes the file and records it;
  - when a local file already exists elsewhere, for example in Downloads, it only copies that file and leaves the location unchanged.

  So after each save, if `location(true)` does not name the kept file, Purple calls the public `setLocation` (verified, `data/data_document.h:159`) and `session().local().writeFileLocation(document->mediaKey(), ...)` (`mediaKey()` verified public, `:295`). Deleting the Downloads copy then leaves offline playback working.
- Playback streams from the kept file, because `createStreamingLoader` prefers a local location (verified earlier, `data_document.cpp:1664-1680`).
- These files sit outside Telegram's media cache, so the cache size and time limits never evict them (the limits are verified at `storage/storage_account.cpp:179-181`). They are also outside its encryption (section 9).
- With the option off, Download all fills the managed cache as pinned-music download does (docs, `docs/purple/pinned_music.md`), subject to those limits.
- A kept file is deleted once its document belongs to no unpurged playlist. Songs in Recently deleted stay until the purge.
- This needs no upstream hook.

**Android.**
- Download all reuses the pinned-music engine. It is first lifted out of the package-private UI classes into a shared Purple class, and it downloads to the path chat playback checks (docs, `pinned_music.md`).
- **Keep downloaded** marks those files as kept forever:
  - one guarded call goes at the end of `CacheByChatsController.lookupFiles` (verified earlier, `messenger/CacheByChatsController.java:146-178`);
  - upstream's automatic cleanup calls `lookupFiles` in both its age loop and its size loop, and skips any file whose `keepMedia` is `KEEP_MEDIA_FOREVER` (verified earlier, `AutoDeleteMediaTask.java:90-101`, `:160-175`);
  - `keepMedia` is package-private in `org.telegram.messenger`, so the Purple helper is a new Purple-owned file in that package, `messenger/PurplePlaylistKeep.java`. It marks every file whose path is in the account's kept list. Purple adds a path when Download all fetches it, or finds it already downloaded, and removes it when the song leaves every unpurged playlist (H21).
- A manual **Clear cache** in Storage Usage still deletes kept files, because the person asked for it.

**Scope (choice P26).** One option covers every playlist, and there is no per-playlist switch. Its default is "keep forever", as the owner chose.

## 12. purple-core versus each client

**purple-core** holds every decision both clients must agree on. It gains new files only, and no shipped sync file is edited.
- `purple/purple_playlists.{h,cpp}`:
  - the peer-token and key grammar, and `QualifySong` with the protected-content flag;
  - format 1 parse, validate and canonical serialize, with partial playlists and `alt`;
  - the stamp rule;
  - the operations with their undo lists, including relink with fallbacks, AutoPurge with its conditions, CopyPlaylist and ImportShare;
  - join and contains;
  - the display view, and chat order for pinned selections;
  - the diff used for notices and import summaries;
  - limits and display-string truncation;
  - playlist ids from caller entropy;
  - parsing and serializing the local sync file, with the memo, `due_seen` and `hidden`.
- `purple/purple_playlists_codec.{h,cpp}`: gzip encode and guarded decode (section 7.4).
- `purple/purple_playlists_sync.{h,cpp}`:
  - the record name, caption and tag, and `IsPlaylistRecordCandidate`;
  - `PlanPlaylistStep`, with the memo inputs, the publish rule, the purge conditions and the cover rule;
  - the containment check used when leaving an old sync chat.
- `purple/purple_playlists_share.{h,cpp}`: building the share file from a snapshot, the send results, the copy map and the exchanged set; parsing it; and `PlanShareImport`.
- Docs: `docs/playlists.md` (format, merge, the cover rule, the publish rule, the automatic purge, the codec, the share file, privacy and logging), plus a README update.
- Tests: `tests/test_playlists.cpp`. The new sources join the explicit source list in `tests/run.sh` (verified earlier, `:31-44`), which also gains `-lz`.

**Each client** owns:
- the files: location, atomic writes, `.bak`, the record cache, and unreadable files;
- mapping its message and document objects to `QualifySong` inputs and source values;
- resolution with fallbacks, and row states;
- the player integration;
- all UI and wording;
- timers, the options' surgical writes, the logout hold and the session-end observer;
- Telegram I/O: search, the fetch by id, the reader's playlist mode, the Purple upload and `sendMedia`, deleting, forwarding, uploading copies, and the import search;
- downloads and keeping.

**Android's bridge.** A new stateless JNI bridge, `jni/purple_jni/playlists_bridge.cpp`, passes JSON and bytes. It can apply an operation, view a library, plan a step, encode and decode, build and plan shares, and read and write the local sync file.
- `exports.map` already exports every `Java_*` symbol (verified earlier). So only the JNI CMake source list and the `purplecore` link list change (section 7.4).
- A stateless bridge copies the whole library across JNI on every operation, and Java holds a JSON string as UTF-16, about twice its size. H19 measures one Add on a 24 MiB library through the bridge on the emulator. If that is too slow, the parsed library stays in native memory per account and only the operation crosses JNI.

## 13. Upstream edit surface

Every line below is marked `// Purple:` and has the rule's shape: one include, one guarded call, one early return, or one delegating row. The single guard condition on Android is called out.

**Desktop:**
- `history/view/media/history_view_save_document_action.cpp:36`: one include and one call (Add, Add album, Open as playlist).
- `media/player/media_player_instance.cpp`: one include and three guarded early returns (`:554`, `:746`, `:763`).
- `media/player/media_player_panel.cpp`: one include and one early return in the `peer` lambda (`:235-265`).
- `media/player/media_player_widget.cpp`: one include and one guarded early return inside the label branch, after `:477-479` (P7).
- `window/window_main_menu.cpp:741-749`: one delegating menu row (P8).
- `menu/menu_item_download_files.cpp:235`, `:261`: one include and one guarded call in each overload (P5 c).
- `info/media/info_media_list_widget.cpp:1376-1381`: one include and one delegating call (P5 c).
- `data/data_file_click_handler.cpp:140`: one include and one guarded early return in `DocumentSaveClickHandler::onClickImpl` (share files, new in this revision).
- `data/data_document_resolver.cpp:239`: one include and one guarded early return (share files).
- `core/application.cpp:979`: one guarded early return (P18).
- `media/player/media_player_listen_tracker.cpp:67-72`: one early return (P20), owned by the user-control design.
- `Telegram/CMakeLists.txt`: the new Purple and core sources.

No new launch hook is needed: the existing Purple call at `window/window_session_controller.cpp:1596` reaches the startup check, and registers the session-end observers. Playlists add nothing to the receipt plumbing in `apiwrap`, `file_upload` or `localimageloader`.

**Android:**
- `ui/ChatActivity.java`:
  - one call at the end of `fillMessageMenu` (`:46020`);
  - two delegating cases in `processSelectedOption` (`:33632`);
  - one guarded early return at the document tap (`:41912-41916`);
  - three calls for chat multi-select (`:10371`, `:19232-19290`, `:3815`).
- `ui/Components/SharedMediaLayout.java:2262-2271`: one call, plus possibly one visibility call.
- `ui/Components/AudioPlayerAlert.java`: one call in `buildSaveOptions` (`:2915`) and one in `showMenuOptions` (`:2869`).
- `messenger/MediaController.java`:
  - one import;
  - **one guard condition** (`:2847`, the rule's "smallest extension point");
  - two early returns (`:2897`, `:1877`);
  - P20's guard condition on the send line (`:7002`), owned by the user-control design.
- `ui/DialogsActivity.java:13843-13955`: one delegating menu row (P8). The launch call at `:2874` already exists.
- `messenger/CacheByChatsController.java:146-178`: one guarded call (Keep downloaded).
- `messenger/MessagesController.java:16215`: one guarded early return (P18).
- `res/values/strings.xml`: new strings in the Purple block.
- `TMessagesProj/jni/CMakeLists.txt`: the Purple block's source list, and `z` in `purplecore`'s link list.

Everything else lives in Purple-owned files, including `PurpleSyncAutoDownload`'s new names and the `appDidLogout` observer. The desktop `docs/purple/playlists.md` names each hooked file and why, as AGENTS.md asks.

## 14. Phased plan

**What comes first:**
- the owner's rule: the refactor that moves existing Purple code out of upstream files (audit workflow wf_31350ea2-5fb) lands before any playlist code. Its send-path decisions (desktop D3, Android C3) decide whether the Purple upload module already exists when phases C and E start.
- The sync-chat layer lands before phases C and E.
- The user-control layer, or the owner's decision on its scope, lands before anything that reads `protectedContentAllowed` as true. Until then, protected songs are not addable and sharing skips them. G-5 waits for it.
- The desktop G1, G2 and G5 fixes land before phase C, extended to `Purple playlists.json.gz` and `*.purpleplaylist`.
- Builds, host tests and emulator runs happen only between 00:00 and 10:30.

**How commits work:**
- Each numbered item is one atomic commit with its docs, using an explicit pathspec.
- Core commits land first.
- Each client bumps its submodule in a commit of its own.
- Push at the end of each phase.

### Phase A: purple-core

- **A1** "Add the playlist library model": the grammar, `QualifySong`, format 1 with partials and fallbacks, stamps, the operations including AutoPurge and ImportShare, undo, join, contains, the view, chat order, limits, the local sync file, `tests/run.sh`, and `docs/playlists.md`.
- **A2** "Compress playlist records with a guarded gzip codec" (zlib, `-lz`, the bomb tests).
- **A3** "Plan playlist sync steps with remembered records and the cover rule".
- **A4** "Build and import playlist share files".
- **A5** "Describe playlists in the README".

**Acceptance for phase A.** All of it is automated, with thousands of seeded runs for the property tests:
- **C1** Join is commutative, associative and idempotent over random full and partial libraries, compared as canonical bytes.
- **C2** Three to five replicas end byte-identical under random operations, random delivery order, duplicated and delayed records, clocks skewed by up to a day, and two replicas sharing one copied file.
- **C3** An operation made after merging a write beats that write. A merge creates no stamp.
- **C4** Concurrent adds of different songs both survive. An album keeps album order.
- **C5** Delete against a concurrent add: the playlist is deleted, `hidden` lists it, and Restore shows both devices' songs.
- **C6** The order cases, as in playlist-design.md. In addition, an order register naming absent songs displays correctly.
- **C7** Undo writes back exactly the undo list, including for relinks with fallbacks and for imports.
- **C8** Random interleavings of publish, prune, merge and search misses across three devices:
  - the join of the surviving records always equals the join of every record ever posted;
  - after two quiet steps per device, exactly one record remains.
- **C9** The validator rejects:
  - unknown keys, and empty playlists;
  - stamps above 2^52, and a `since` above its stamp;
  - bad or duplicate ids;
  - an `alt` with more than 3 entries, with duplicates, or containing the primary;
  - purged ids still present.

  It accepts partial playlists, and order ids absent from `songs`.
- **C10** No payload or share file that core can build contains `access_hash`, `file_reference` or a numeric self user id.
- **C11** Purge:
  - a purged playlist never returns;
  - a record holding it becomes covered;
  - `hidden` carries the pre-merge playlist.
- **C12** A format 2 record or share file is neither merged, pruned nor imported. Nothing is serialized over a NewerFormat or Invalid local library.
- **C13** `IsPlaylistRecordCandidate`, the shipped `IsSyncHistoryCandidate` and the share-file detector are pairwise disjoint over:
  - records, settings records and share files;
  - a text marker carrying `#purplechatsync`;
  - share files whose playlist titles contain `#purplesync` or `#purpleplaylists`;
  - forwarded copies of each.
- **C14** Golden files: fixed inputs give fixed canonical bytes. The Android bridge test runs the same files.
- **C15** The local sync file round-trips and keeps unknown keys.
- **C16** The codec round-trips. A 32 MiB + 1 expansion is Oversized, with peak output memory at most the cap plus one chunk. A second member, trailing bytes, a bad CRC or a missing magic is Invalid.
- **C17** The memo:
  - a search that misses a present record, confirmed by id, plans no publish and no prune;
  - a record confirmed gone is dropped;
  - a non-valid entry from an older classifier version is re-read;
  - a transient failure is never memoized.
- **C18** The publish rule: a step publishes exactly when no present valid record's hash equals the merged library's, and never above the caps.
- **C19** Steady state: the same candidates, with a library equal to one of them, plan no download, no publish and no prune.
- **C20** AutoPurge:
  - it purges at s + 30 days and not before;
  - each of the four conditions, alone false, prevents it;
  - a stale library whose record set lost its covering record to search lag (a memo record gone, nothing new) does not purge;
  - a restore merged in the same step prevents it;
  - a concurrent Restore and AutoPurge resolve to the purge, with `hidden` offering Keep a copy.
- **C21** Relink pushes the old primary into `alt`, deduplicates it, and caps it at 3. Remove drops `alt`.
- **C22** Share build:
  - no `peer`, `msg`, `un` or `alt` appears;
  - copies are substituted in `songs`, `order` and tombstones;
  - left-out songs are absent from both `songs` and `order`;
  - tombstones appear only for documents in the exchanged set;
  - the caption builder emits no title and no `#`.
- **C23** ImportShare:
  - adopts only larger stamps, and keeps local values on ties;
  - restores a deleted playlist;
  - sends a purged id to "new";
  - refuses stamps a day ahead;
  - is idempotent when repeated;
  - gives the same visible songs, title and order for two files imported in either order;
  - maps copies to originals and adopts only tombstones for them;
  - has exact undo.
- **C24** `QualifySong` with the protected-content flag on and off.
- **C25** A local library file larger than 32 MiB loads and merges. A snapshot above the caps is refused with a reason.

### Phase B: desktop playlists, local only

- **B1** "Update purple-core for playlists" (the submodule bump and CMake sources).
- **B2** "Store each account's playlists beside its data".
- **B3** "Resolve playlist songs, falling back to earlier copies".
- **B4** "Add songs, albums and selections to playlists from song menus" (the save-action hook and the three selection hooks).
- **B5** "Show and edit playlists from the main menu" (the boxes, the painted list, Recently deleted with dates, recent-use order, and the `menu_item_p` row).
- **B6** "Play a playlist in its own order".
- **B7** "Save pinned songs as a playlist in chat order".
- **B8** "Download playlists and keep them past the cache limits".
- **B9** "Document desktop playlists".

**Acceptance** runs on the disposable account, in an isolated instance, at night. the owner verifies anything that needs the screen.
- **D1** Adding songs:
  - from a chat, a topic, Saved Messages, a channel, a migrated group's old half, shared media, the hover list, and multi-select in a chat and in shared media;
  - a forwarded copy relinks in place, and the old copy becomes a fallback;
  - with the protected-content flag false, the item is absent in a protected chat. With it true (once the user-control layer exists), a protected chat's song adds;
  - record whether the playing song reports `loading()`.
- **D2** Playing:
  - songs play in order, including next, previous, auto-advance, media keys and OS controls;
  - the modes behave as named;
  - leaving playlist mode, the hover panel, and the track-name click behave as specified. A click elsewhere on the widget does not open the playlist;
  - count upstream's shuffle-list requests;
  - a 2,000-song playlist opens and drags smoothly.
- **D3** Deleting a source makes the row fall back to an earlier copy when one exists, and shows Deleted otherwise. **Use this copy** relinks it.
- **D4** After a restart the library is intact and channel songs resolve (H2). A NewerFormat file stays byte-identical.
- **D5** Save as playlist uses chat order and reports skipped files.
- **D6** Download all with Keep downloaded on:
  - plays offline after a restart (H22);
  - survives "Clear cache" in Local Storage;
  - after a song had also been saved to Downloads, deleting the Downloads copy and going offline still plays it.

  With the option off, the files follow the cache limits.

### Phase C: desktop sync

Before this phase, run the spike on the disposable account: H1, H3, H8 to H14, H19, H25 (for the sync-channel design), H26 and H27.

- **C-1** "Find and read playlist records in the sync chat": the search with full paging, the author filter, the fetch by id, the reader's playlist mode and the codec.
- **C-2** "Send playlist records through a Purple upload", unless the refactor already built it.
- **C-3** "Sync playlists through the sync chat": the step with the memo and cache, the publish rule, pruning, the automatic purge, the timers and checks, the options, the notices, the sync line, the first-run bulletin and the other-readers confirmation.
- **C-4** "Move playlist records when the sync chat changes".
- **C-5** "Keep or sync playlists at logout": the hold, the dialogs and the session-end observer.
- **C-6** "Describe playlist sync". `playlists.md` gains its sync section. In `account_sync_plan.md`, the "Playlists later" section and the lines describing library records inside the settings layer are replaced (`:104`, `:415`, `:421-433`, `:478`). `sync.md` loses its stale Android item (`:88-91`).

**Acceptance:**
- **S1** The first step publishes a snapshot. An edit publishes within 60 s. Ten edits spread over 9 minutes publish once, within 10 minutes of the first edit.
- **S2** A check with nothing new sends exactly one request (counted from Purple's own counters, not from logs that name content) and writes nothing.
- **S3** Three records are left alone:
  - a hand-posted malformed record is reported and never deleted;
  - a record posted by another member of a test group is ignored;
  - a record over 10 MiB compressed is reported as oversized without being downloaded.
- **S4** A second instance with a copied `tdata`: after both have stepped twice, one record remains.
- **S5** A delete that failed (the network was cut after the publish) is retried and succeeds at the next step.
- **S6** Purges:
  - a 30-day purge on a playlist whose deletion stamp is set in the past, through a test-only hook in the core tests rather than the app;
  - a purge by a second instance;
  - an instance started from a `tdata` copied before a Restore does not purge the restored playlist.
- **S7** Logout:
  - with sync on, the files are deleted only after the logout completes;
  - cancelling at upstream's download prompt keeps them;
  - with sync off, the warning appears;
  - offline with changes waiting, the retry dialog appears;
  - with no sync chat, the setup dialog appears;
  - a forced logout deletes the files.
- **S8** Changing the sync chat publishes a snapshot into the new chat and deletes the old chat's records.
- **S9** A test group with a second member holds the first publish until confirmed. A public channel is refused.

### Phase D: Android playlists, local only

- **D-1** "Bridge the playlist library to Java".
- **D-2** "Keep a Purple playlist's order in the music player" (the four `MediaController` edits and the endReached params).
- **D-3** "Store playlists and resolve their songs".
- **D-4** "Add songs, albums and selections from the chat, shared media and the player".
- **D-5** "Show, edit and play playlists from the chat list menu and Settings".
- **D-6** "Save pinned songs as a playlist and play the pinned list in chat order".
- **D-7** "Download playlists and keep their files past Keep Media".
- **D-8** "Document Android playlists".

**Acceptance** runs on the local arm64 emulator at night.
- **A1** D1's cases, from the chat menu, the chat and shared-media action bars, and the player sheet, including the sheet's Add to playlist for a protected playing song. The picker over the sheet leaves the sheet open.
- **A2** Playing:
  - Play prepares, then the sheet lists the playlist in order with the "Music" title and chat lines on each row;
  - next, previous, the notification and the lock screen follow it (H5);
  - Repeat One and stop-at-end work, and no chat music loads (H24);
  - songs with equal ids from different chats both play;
  - starting on the playing song resumes it;
  - a song that fails to resolve is skipped.
- **A3** D3's cases.
- **A4** After a force-stop, the library is intact and the queue is gone.
- **A5** The pinned list of a migrated group plays, and saves, in chat order.
- **A6** Kept files survive the age cleanup and the size cleanup (H21). A song that was only played is not kept.

### Phase E: Android sync

- **E-1** "Find and read playlist records in the sync chat", plus the new names in `PurpleSyncAutoDownload`.
- **E-2** "Send playlist records through a Purple upload", unless the refactor already built it.
- **E-3** "Sync playlists through the sync chat".
- **E-4** "Move playlist records when the sync chat changes".
- **E-5** "Keep or sync playlists at logout".
- **E-6** "Document Android playlist sync".

Before this phase, run H7, H11, H26 and H27 on the emulator. Acceptance: S1 to S9 on the emulator.

### Phase F: cross-client acceptance

There is no code in this phase. Results go into the docs, in one commit per repository.
- **X1** Create on desktop and sync automatically. Android gets the same playlist in the same order, and it plays. DM, basic-group and channel songs resolve on a session that never opened those chats (H1).
- **X2** Both devices offline: each adds a different song and renames the playlist. After reconnecting, both songs are on both devices, with one title.
- **X3** Deletion, Restore, Delete forever with Keep a copy (also after a restart), and the 30-day purge.
- **X4** After quiet steps, the sync chat holds exactly one record.
- **X5** A corrupted record is reported on both devices and never deleted. Settings checks are unaffected.
- **X6** Download every record posted in X1 to X4 and scan it for access hashes, file references, the user id, paths, device names and settings content. After a Remove, a Delete forever and the next sync, the removed song's chat and the purged title appear in no record. Scan both clients' logs for titles, peer tokens and document ids.
- **X7** A private-channel song shows No access after the channel is left, and falls back when a fallback exists.
- **X8** A shipped-build device checks settings while records exist, and reaches its normal verdict.
- **X9** Clearing Android's app data restores the library from the sync chat.
- **X10** Steady state: an hour of idle app starts sends one search per check and no downloads.

### Phase G: sharing

- **G-1** "Share playlists by forwarding their songs and a playlist file" (desktop).
- **G-2** "Open shared playlist files as playlists" (desktop, both tap hooks).
- **G-3** and **G-4**: the same two on Android.
- **G-5** "Upload copies of protected songs when sharing, after asking" (both clients). This waits for the user-control layer. Until then G-1 and G-3 ship "skip protected songs, and say so".
- **G-6** "Document sharing".

**Acceptance**, on the disposable account, with a second account or a bot where one is needed:
- **SH1** Share a 30-song playlist from three chats to a DM. There is no header or caption, the order holds (H14), there is one notification, and the file comes last.
- **SH2** The recipient imports it. All songs are found (H20) and play, and the file holds no source chat or message id (a scan).
- **SH3** The sharer adds two songs, removes one and renames the playlist, then re-shares "only new". The recipient's Merge adds two and removes one. The title follows the stamp, and the recipient's own earlier addition survives.
- **SH4** The recipient re-shares to the sharer:
  - on the sharer's device that made the copies, the merge adds no protected duplicates;
  - on the sharer's other device, the duplicates appear, as documented.
- **SH5** Protected songs (after the user-control layer): the dialog's counts and sizes are right, Upload copies yields unprotected copies (H18), and Skip leaves them out. With the flag false, they are skipped with the note.
- **SH6** A 500-song share under flood waits resumes after a forced quit (H16, H17).
- **SH7** A playlist titled "#purplesync test", shared to Saved Messages of the home account, leaves the next settings Check at its normal verdict.
- **SH8** Slow-mode, paid-message and no-rights chats are refused in the chooser.
- **SH9** A non-Purple client (official desktop) shows plain songs and a harmless file.
- **SH10** Import on two recipient devices before they sync converges to one playlist.
- **SH11** On desktop, a first tap on an undownloaded share file opens the import, and nothing appears in Downloads.

### Later (each with a trigger)

- **Delta records.** Trigger: H19 or real use shows snapshot traffic matters (P30).
- **Remove downloads** per playlist. Trigger: the owner asks.
- **Following share updates automatically.** Trigger: the owner asks. It needs a search per followed chat.
- **Edit in place** for records.
- **The Android Auto browse tree.**
- **A re-assert operation** that replays a `.bak` as new writes.
- **Timer-message songs**, through the user-control design's kept copies.
- **A synced copy map.** Trigger: duplicates from cross-device protected re-shares bother the owner.

## 15. Risks

- **Deleting messages is new for Purple.** A bug in contains could delete unmerged data. The mitigations:
  - exact canonical comparison in core;
  - pruning only valid format 1 records that a record read in the same step covers;
  - the proof in section 8.6, backed by C8;
  - every device's library still holds its own data, and republishes it at its next step;
  - H3 is checked before phase C.
- **The memo could hide a record.** It is keyed by message id and document id, a miss is confirmed by id, non-valid statuses are re-read after an upgrade, and it is cleared on Start over. C17, C19 and S2 test it.
- **Concurrent snapshots** from two devices cost one extra upload each, and the next step merges and prunes them.
- **Automatic purge on stale or lagging data.** It is guarded by running after the merge, complete discovery, the newer-format check, the request-proven clock, and the 15-minute second sighting (section 7.9). The residual case is a restore within about 15 minutes of the purge, which is concurrent. C20 and S6 test it.
- **Other readers of a sync chat** see records and could post fakes. The author check (H11), the playlist confirmation, the public-chat refusal and the sync-chat layer's warnings mitigate this.
- **gzip bombs**, from records or share files. The streaming guard (C16) handles them.
- **Large libraries on phones.** H19 decides whether the stateless bridge stands.
- **Upstream merges** touch the files listed in section 13. Each hook is one line, and `MediaController.java` keeps a single guard condition. Playlists add nothing to the upstream send code.
- **Dependencies not yet decided:**
  - the user-control layer, which stopped: P13 and G-5 wait for it;
  - the refactor's send-path decisions (D3, C3);
  - the sync-chat layer.
- **Search reliability** (H10, H13, H20, H25). Lag does no harm, and total failure has fallbacks.
- **Sharing spam and flood.** Large shares can take minutes, and Telegram may throttle. The confirmation warns, and the job persists.
- **Protected content.**
  - Copies spread content its sender protected. Telegram's content-protection guidance asks clients to disable forwards and downloads for such messages (docs).
  - The terms research records that enforcement falls on the api_id owner.
  - Copies sit behind a user-control setting and an explicit dialog that says the copies are unprotected. the owner decides the scope.
- **Hostile share files** can only affect the one playlist they name. Every import is previewed and undoable, and future stamps are refused.
- **Songs rot.** Channels delete posts. Fallbacks and relinks soften this.
- **Clutter.** Records live in the sync chat, not Saved Messages. The publish rule, pruning and silent posts bound this to about one record.
- **Plain local files.** The library and kept audio sit outside Telegram's passcode-encrypted storage, as other Purple files do. The docs say so.

## 16. Deliberately left out

- Live receive (merging on message arrival), edit in place, and automatic share updates.
- Delta records in v1 (P30).
- Any link to settings sync: spaces, installs, sequence numbers, clone checks, ledgers and device lists.
- Collecting removed-song tombstones beyond purges.
- Per-playlist shuffle or repeat, manual list order, covers, descriptions, folders and smart playlists.
- The same song twice in one playlist.
- Audio without a music attribute, and secret-chat, scheduled, story, poll-media, profile-music and timer songs.
- Automatic relinking, and writing a working fallback back as the primary.
- Import or export to upstream Profile music.
- An Info-layer playlist section, and the Android Auto tree.
- A per-playlist Keep downloaded switch (P26), and Remove downloads.
- A desktop selection top-bar button (P24).
- Unlink and a follow switch for shared playlists.
- A configurable check interval.

## 17. Alternatives considered and rejected

- **Playlist records inside the settings record layer.** It needs a change to the shipped verdict, isolation flags, a space rule, sequence reservation and clone checks.
- **Each playlist as a private channel.** Order is post order, protected songs cannot be forwarded, and every playlist is one more chat.
- **Playlists as a section of `settings.toml`**, **upstream Profile music**, **Saved Messages tags**, **fake history items**, **Android negative-id queue items**, **mutating `getPlaylist()` directly**, and **one record per playlist**: all rejected for playlist-design.md's reasons.
- **Delta records with a dirty set and compaction triggers** (revision 2). This was more machinery than the decisions asked for, and it kept removed songs' chats in the cloud for up to a week. The memo already makes the steady state one search.
- **A cloud-view library diffed against the local one.** It needs another large file and a recompute after deletions.
- **Automatic purge before the merge, or from the Playlists screen.** A stale device could purge a playlist restored weeks earlier.
- **Purging only when the record carrying the winning deleted register was read in this step** (critique 1.1's second option). A stale device's own fresh snapshot satisfies it, so it does not stop the lag case. The 15-minute second sighting does.
- **Desktop `unixtime::updates()` as the clock gate.** It never fires on a correct clock.
- **A file-backed desktop download for records above 10 MiB.** It needs a Purple download loop or a temporary file for a size no realistic library reaches. It stays open as P29 (b).
- **The existing receipt-threaded send paths** for records. They need upstream edits that the refactor is removing, and Android's cannot send silently.
- **Deleting files in the logout hook.** Upstream's later prompts can cancel the logout.
- **Bypass hooks around every upstream protection gate on desktop.** That means four or more extra hooks, while the user-control layer's root switch opens them all together. The gates are listed as a contract instead (section 3.3).
- **Resolving ahead during playback on Android.** Unresolved songs cannot sit in upstream's queue. Calling `setPlaylist` again per batch would reshuffle with shuffle on.
- **A synced copy map** (critique 1.7's preferred fix). It adds a field to the source value, for a rare round trip. The local map covers the device that made the copies.
- **A separate share id** (section 10.2). Concurrent imports on two recipient devices would leave two playlists with one share id.
- **Merging fallbacks as a bounded set** (section 7.7). It breaks associativity against removals.
- **A snapshot or delta kind field in records.** Correctness never needs it.
- **Destination message ids in share files.** In private chats and basic groups, each side numbers messages separately.
- **`messages.sendMedia` with the existing document instead of forwarding.** Docs: it works for unprotected media only, one document per request, or albums of up to 10 through `sendMultiMedia`, which would regroup songs. Forwarding carries 100 per request, as the owner chose.
- **Compressing the local library file.** It saves little and makes the file uninspectable.
- **An automatic copy on purge.** Without causal metadata it would bring back deliberate deletions.
- **An Android `clearPlaylist()` hook** for ending playlist mode. Identity comparison against the public `getPlaylist()` needs no edit.
- **Every tombstone in every share file.** It reveals document ids of songs that never left the device.
- **`#purplesyncchat` as the marker tag.** It contains the shipped settings tag as a substring.

## 18. Hypotheses to verify

Each stays unverified until run. Results go into `docs/purple/playlists.md`, without account identifiers.

- **H1** `messages.getMessages` with bare ids returns private-chat and basic-group messages on a session that never opened those chats.
- **H2** Desktop resolves a channel song after a restart, once the dialog list loads. `contacts.resolveUsername` resolves an unjoined public channel. A left private channel returns `CHANNEL_PRIVATE`.
- **H3** A session can delete a record another session of the account posted:
  - in Saved Messages with `messages.deleteMessages` and `revoke`;
  - in a channel with `channels.deleteMessages`;
  - its own messages in a group;
  - records in an old sync chat after a change.

  The deletion reaches the other sessions on both clients.
- **H4** With private listening on, neither client sends `messages.reportMusicListen` for playlist or chat playback. Turning it off in the middle of a song reports nothing for the earlier part. With it off, playlist playback reports as chat playback does.
- **H5** On Android, the player sheet, the notification and the lock-screen controls follow the ordered queue, with shuffle on and off.
- **H6** Retired: shipped builds ignore playlist records (verified in source; checked live as X8).
- **H7** Android's `getCurrentTimeMillis()` includes the server time difference.
- **H8** Whether official clients and shipped Purple Android auto-download a 0.1 to 10 MiB `Purple playlists.json.gz`, and share files, at default settings.
- **H9** Desktop streams a song whose message was loaded only through `requestMessageData`, and refreshes its file reference after it expires.
- **H10** `messages.search` in the sync chat (a private channel, a supergroup with `from_id` set to self, and Saved Messages), with `q = "#purpleplaylists"` and the document filter, returns every record within a minute of posting. Record whether short pages occur before the end. Test the file-name query too.
- **H11** The `out` flag is set on messages this account sent, including broadcast-channel posts, as seen from every session of the account, and on no one else's, including other admins' posts.
- **H12** `from_id` set to self in supergroup and basic-group searches returns only this account's messages.
- **H13** Tag matching:
  - `q = "#purplechatsync"` and `q = "#purpleplaylists"` each return only messages carrying that exact tag;
  - record whether `q = "#purplesync"` also returns longer tags that begin with it.
- **H14** `messages.forwardMessages`:
  - accepts 100 ids and refuses 101 (record the error);
  - the destination order follows the id vector;
  - `drop_author` with `drop_media_captions` leaves no header and no caption on music;
  - `silent` suppresses notifications.
- **H15** Album songs forwarded together in one batch stay grouped. Songs from different albums or chats do not merge into a group.
- **H16** Flood behaviour when sharing 500 and 2,000 songs to a DM and to a group: requests before the first FLOOD_WAIT, typical waits, total time.
- **H17** Re-sending a forward batch with the same random ids after a lost response creates no duplicate.
- **H18** A re-uploaded protected song:
  - keeps title, performer, duration and cover from its tags;
  - has no `noforwards`;
  - is matched by the music filter;
  - leaves no copy in desktop's Downloads.
- **H19** Parse, join and serialize time and peak memory for 7,200-entry, 24 MiB and 32 MiB libraries, in core's host tests on the Mac (at night) and through the JNI bridge on the emulator, including one Add on a 24 MiB library through the stateless bridge. A 7,200-entry step stays under one second.
- **H20** The import search (music filter, `from_id` set to the sender, `max_id` set to the file, `top_msg_id` for topics) finds the forwarded songs in DMs, basic groups, supergroups, forum topics and channels.
- **H21** Android's age and size cleanups skip files marked through the `lookupFiles` hook.
- **H22** Desktop plays a Download-all song from the Purple folder after a restart with the network off, including after an earlier Downloads copy was deleted.
- **H23** Retired: desktop's `base::unixtime::updates()` does not fire on a correct clock (verified in source). The purge now waits for a successful request instead (docs, mtproto/description).
- **H24** Android `setPlaylist` with `loadMusic = true` and Purple's `endReached` params:
  - loads no chat music;
  - sends no `loadMoreMusic` request;
  - honours Repeat One and stop-at-end;
  - shuffles.
- **H25** From a fresh session, `messages.searchGlobal` with `q = "#purplechatsync"` finds a text marker this account sent in:
  - its private Purple channel;
  - a basic group;
  - a supergroup;
  - Saved Messages;
  - an archived chat, with `folder_id` unset.

  Record how long after posting. The result goes to the sync-channel design.
- **H26** Desktop: `account->sessionChanges()` fires `nullptr` with `!Core::Quitting()` on a user logout, a forced logout and a forgotten-passcode logout, and never on quit. Android: `appDidLogout` fires for `performLogout` types 0, 1 and 2 on that account's `NotificationCenter`.
- **H27** The Purple upload:
  - small parts for a record, and big-file parts for an audio file over 10 MB;
  - followed by `messages.sendMedia` with `silent` and a Purple random id;
  - with the returned Updates applied through `applyUpdates` or `processUpdates`.

  It shows the message normally on the sending device, yields its message id and document id, notifies no one, and leaves no copy in desktop Downloads or Android's Telegram Files.
- **H28** On desktop, `DocumentSaveClickHandler::onClickImpl` is the tap path for an undownloaded share file in a chat, a topic, shared media's Files tab and search results. No other entry point writes it to Downloads before the Purple hook sees it.

## 19. Sources

- **purple-core `9c33c53`:**
  - `purple/purple_sync_inventory.{h,cpp}` (`h:27`, `cpp:45-132`);
  - `tests/run.sh` (1-60).
- **tdesktop `32bea43ed5`:**
  - `AGENTS.md` ("Keeping upstream merges easy");
  - `docs/purple/keep_media.md`, `pinned_music.md`, and `config.md` (34-110, 231-300);
  - `core/application.cpp` (971-1020, 2005-2030);
  - `main/main_account.h` (70-90) and `main/main_account.cpp` (50-70, 200-240, 520-560);
  - `main/main_session.cpp` (304-315);
  - `lib_base/base/unixtime.h` (10-35) and `lib_base/base/unixtime.cpp` (20-170);
  - `mtproto/session_private.cpp` (1665-1690, 2095-2120);
  - `mtproto/details/mtproto_dc_key_creator.cpp` (665);
  - `storage/file_download.h` (26-32) and `storage/file_download.cpp` (108-118);
  - `purple/purple_sync_candidate_reader.cpp` (20-30, 110-175);
  - `purple/purple_sync_config_post.cpp` (85-175);
  - `purple/purple_sync.cpp` (290-330);
  - `apiwrap.h` (157-163, 362-374), and commit `f7f536d165` (`--stat`);
  - `apiwrap.cpp` (525, 3793-4030);
  - `history/view/media/history_view_document.cpp` (1440-1460);
  - `data/data_file_click_handler.cpp` (70-175);
  - `data/data_document_resolver.cpp` (228-245);
  - `data/data_document.cpp` (1195-1250, 1414-1425, 1660-1690) and `data/data_document.h` (112, 159, 192, 295);
  - `core/file_location.cpp` (78-98);
  - `history/view/media/history_view_save_document_action.cpp` (25-75, 117-128) and its header (29, 35);
  - `history/history_inner_widget.cpp` (3080, 3105-3120, 3400-3412, 3708-3720);
  - `history/view/history_view_context_menu.cpp` (259-275, 335-360, 990-1008, 1125-1136);
  - `info/media/info_media_list_widget.cpp` (1281, 1300-1385);
  - `menu/menu_item_download_files.cpp` (228-290);
  - `history/view/history_view_top_bar_widget.h` (225-245) and `.cpp` (122-157, 1178-1210, 1350, 1527);
  - `media/player/media_player_widget.cpp` (470-495);
  - `media/player/media_player_listen_tracker.cpp` (60-96);
  - `storage/storage_account.cpp` (176-182, 1828-1840);
  - `data/data_peer.h` (335-340);
  - `boxes/share_box.cpp` (1971);
  - `window/window_peer_menu.h` (200-225), `window/window_main_menu.cpp` (739-750), `window/window_session_controller.cpp` (1585-1600);
  - `mtproto/scheme/api.tl` (305, 309, 2463, 2466, 2470, 2471, 2507, 2549, 2706, 2730, 2732, 2766);
  - `Telegram/CMakeLists.txt` (30, 89).
- **Android `cb6d43f92`:**
  - `messenger/MediaController.java` (1826-1993, 2733-2826, 2836-2975, 4402, 6808-6835, 6985-7010);
  - `messenger/MessagesController.java` (6765-6782, 7316, 16215-16264, 17949);
  - `messenger/SendMessagesHelper.java` (2540-2600, 9785-9800);
  - `messenger/CacheByChatsController.java` (140-215) and `messenger/AutoDeleteMediaTask.java` (19-195);
  - `messenger/ChatObject.java` (2188, 2195);
  - `messenger/MessageObject.java` (11895-11912, 12212);
  - `tgnet/ConnectionsManager.java` (295-313);
  - `ui/ChatActivity.java` (1255, 3812-3830, 10318-10387, 19225-19300, 33632, 41895-41925, 46020);
  - `ui/Cells/ChatMessageCell.java` (17858-17920);
  - `ui/Cells/AudioPlayerCell.java` (136-146);
  - `ui/Components/AudioPlayerAlert.java` (1345-1356, 2490-2500, 2533-2542, 2865-2925, 2970-2980);
  - `ui/Components/SharedMediaLayout.java` (2216-2271);
  - `ui/DialogsActivity.java` (1366, 2874, 3509, 13843-13955);
  - `ui/MainTabsActivity.java` (93-103), `ui/LaunchActivity.java` (587), `ui/LogoutActivity.java` (167);
  - `TMessagesProj/jni/CMakeLists.txt` (215-300).
- **Telegram documentation (read 2026-09-30):**
  - `core.telegram.org/api/search`: the Chats-tab description of `messages.searchGlobal`, "max one message per peer", `channels.searchPosts` over public channels, and no statement on prefix matching;
  - `/method/messages.searchGlobal`: "Search for messages and peers globally" and its filters, with no statement of scope;
  - `/mtproto/description`: messages over 300 s in the past or 30 s in the future are ignored;
  - `/api/files`: the 10 MB part-method threshold, the part sizes, and resending only non-protected media;
  - `/method/messages.forwardMessages`, `/method/messages.search`, `/api/errors`, and `/api/content-protection` (through user-control-research-terms.md);
  - carried from playlist-design.md: `/api/peers`, `/api/updates`, `/api/file-references`.
- **Decisions and coordination:**
  - playlist-decisions-r1.md and playlist-decisions-r2.md (with round 2b);
  - playlist-product-choices.md (P8 and P21 wording);
  - the coordinator's checkpoint (the user-control stop, 20:10);
  - refactor-plan-desktop.md (B16, D3) and refactor-plan-android.md (C3);
  - user-control-research-terms.md;
  - the relayed request of 2026-09-30.
- **Measured:** revision 2's `libsize.py`, and this pass's `libsize2.py` in the scratchpad (500, 2,000 and 7,200 synthetic entries).

## 20. Critique responses

Each point was checked in code or docs. "Accepted" means the design changed as described. Points are numbered by critique (1 or 2) and by their order in it.

**First critique**

- **1.1 The automatic purge ran before the merge (major). Accepted.** The logic holds as stated: step 1 purged from a stale local view before the search. The purge now runs only inside the step, on the merged library, behind four conditions (section 7.9). I took the critique's first option, requiring two sightings at least 15 minutes apart. I rejected its second option (require the record carrying the winning register to have been read in the step), because a stale device's own newly published snapshot satisfies it (section 17). I added two conditions of my own: no present newer-format record, and no memo record vanishing without a new record appearing. C20 and S6 are rewritten.
- **1.2 Desktop logout deleted files before upstream's cancellable prompts (major). Accepted.** Verified at `core/application.cpp:979-999`. Deletion now happens in a session-end observer:
  - `sessionChanges()` is verified public at `main/main_account.h:79`;
  - `Core::Quitting()` is verified at `core/application.cpp:2017`, and is needed because the destructor also ends the session (`main/main_account.cpp:60`);
  - on Android, `appDidLogout` is posted after `clearConfig` (verified, `MessagesController.java:16231`, `:16247`).

  The hold's flag is one-shot. H26 is new. The same observer gives L1 (now P27) its mechanism.
- **1.3 The clock gate would never open on desktop (major). Accepted.** Verified at `lib_base/base/unixtime.cpp:139-168`: `update()` ignores shifts under 3 s, applies once per process unless forced, and fires only on change. The gate is now "a request in this step succeeded". MTProto ignores `msg_id` values over 300 s in the past or 30 s in the future (docs), and a bad `msg_id` forces a correction (verified, `session_private.cpp:2115-2118`). H23 is retired. H11 now sits in the phase C and E spike lists, and Android's time question stays as H7.
- **1.4 The desktop reader would crash above 10 MiB (major). Accepted, by the critique's alternative fix.** Verified:
  - the `Expects` is at `storage/file_download.cpp:115`;
  - `kMaxFileInMemory = 10 * 1024 * 1024` is at `file_download.h:30`;
  - the reader's `save(..., QString())` is at `purple_sync_candidate_reader.cpp:162-164`.

  Records are now capped at 10 MiB compressed and 32 MiB decompressed, with local edits at 24 MiB. The desktop refactor plan independently recommends the same 10 MB cap (D3). A file-backed download remains open as P29 (b). S3 gains the over-cap case.
- **1.5 Record posting relied on receipt plumbing in upstream files (major). Accepted.** Verified:
  - `apiwrap.h:370`;
  - commit `f7f536d165` (12 files, +220 -52);
  - Android `SendMessagesHelper.java:9789` with `notify` hard-coded true (`:9793`).

  Playlists now send through a Purple-owned upload and `messages.sendMedia` (section 8.2), which the desktop refactor plan recommends as D3. The dependency on D3 and C3 is named in sections 8.2, 13 and 14. H27 is new.
- **1.6 A first tap on an undownloaded share file went to the save handler (major). Accepted.** Verified:
  - `history_view_document.cpp:1450-1454` routes to `_savel` unless loaded or playable;
  - `data_file_click_handler.cpp:74-115` computes a save name and saves.

  One early return goes in `DocumentSaveClickHandler::onClickImpl` (`:140`), leaving "Save As" alone. `*.purpleplaylist` joins the G1/G2 and Android auto-download exclusions. On Android, the first tap downloads through `didPressButton` (verified, `ChatMessageCell.java:17858-17870`), which is harmless and stated. SH11 and H28 are new.
- **1.7 Protected copies duplicate on a round trip (major). Accepted in part.** The logic holds. The copy map in share history is now used on import too (copies mapped back, only tombstones adopted) and for tombstones when building a file, on the device that made the copies. I rejected the synced copy map: it needs a new source-value field for a rare case, and it is listed under Later with a trigger. SH4 is restated for both devices, and C22 and C23 test the mapping.
- **1.8 A title containing `#purplesync` in the caption triggers settings review (minor). Accepted.** Verified at `purple_sync_inventory.cpp:64-70` and `:118-132` (core `9c33c53`). The caption now has no title and no `#`. C13 and SH7 cover it.
- **1.9 The `searchGlobal` scope came from UI guidance (minor). Accepted.** Checked on core.telegram.org today: the sentence is in the Chats-tab guidance next to "max one message per peer", and the method page states no scope. The claim is relabelled as H25 and handed to the sync-channel design.
- **1.10 Memo churn, stale statuses and early paging stop (minor). Accepted.** Misses are confirmed by id, statuses carry the classifier version, transient failures are not memoized, and paging runs to an empty page or `count` (sections 8.3 and 8.4, C17). The churn scenario also disappears under the new publish rule, because a present record confirmed by id still equals the library.
- **1.11 The library and dirty set lived in two files, and Keep a copy relied on `.bak` (minor). Accepted.** The dirty set no longer exists (2.4). What to publish is decided from the atomically written library alone. The pre-merge content of hidden playlists lives in `hidden` in the local sync file until the notice is dismissed (sections 7.1 and 7.8).
- **1.12 The 64 MiB cap on the local file, and the 48 MiB arithmetic (minor). Accepted.** The read cap now applies only to records and share files, and the local file has no cap (section 7.3, C25). The rationale for the local-edit cap is corrected, and the old claim is withdrawn in section 7.4.
- **1.13 The list of P13 gates was incomplete (minor). Accepted in part.** Verified:
  - `history_inner_widget.cpp:3407`, `:3714` and `:3111-3113`;
  - `history_view_context_menu.cpp:997-1000`;
  - `history_view_save_document_action.cpp:117-124`;
  - `info_media_list_widget.cpp:1315`;
  - `AudioPlayerAlert.java:2877`, `:2878`, `:2977`, `:2539`.

  Section 3.3 now lists every gate as a contract for the user-control layer, which is expected to lift protection at its root. On Android, the existing `showMenuOptions` hook adds "Add to playlist" whenever upstream hides Save To, so no extra hook is needed. I rejected adding bypass hooks around each desktop gate, because that means four or more extra upstream lines for a layer whose own root switch opens them together.
- **1.14 "The move waits" cannot work on Android (minor). Accepted.** Verified at `MediaController.java:2948-2975`. Android now resolves the playlist before Play, with a progress state, and skips failures. Desktop keeps resolving ahead, because Purple owns its navigation (sections 5.1 and 5.3).
- **1.15 The sheet reads the global-search params (minor). Accepted.** Verified:
  - `MediaController.java:6831-6833`;
  - `AudioPlayerAlert.java:1345`, `:1352`, `:2497`;
  - `AudioPlayerCell.java:139-143`.

  The effect is stated as desirable, and A2 checks it on screen.
- **1.16 Two hook sites were imprecise (minor). Accepted.** Verified:
  - `media_player_widget.cpp:476-481`: the hook moves inside the label branch;
  - `MediaController.java:6992-7005`: the P20 guard wraps the send line at `:7002`, so the resets run. The site is owned by the user-control design, but named here.
- **1.17 `save(origin, path)` does not always move the location (minor). Accepted.** Verified at `data_document.cpp:1202-1225`, with `setLocation` public at `data_document.h:159`. Keep downloaded now records the kept file as the location. Protected copies never save to a temporary path (sections 10.5 and 11). D6 and H22 gain the Downloads-copy case.
- **1.18 C23's order independence did not follow (minor). Accepted.** C23 now tests that visible songs, title and order do not depend on import order, and states that sources may. I did not adopt the join tie-break, because it would move a recipient's own source (with its fallbacks) to a DM message on a re-share back.
- **1.19 Share files reveal more than disclosed (minor). Accepted.** Tombstones are limited to the exchanged set. The confirmation and the disclosure say that the file shows when each song was added (sections 9, 10.3 and 10.6).
- **1.20 The stateless JNI bridge copies the whole library per operation (minor). Accepted as a measurement.** H19 now covers one Add on a 24 MiB library through the bridge, with a native per-account library as the named fallback. The lower caps also halve the worst case.

**Second critique**

- **2.1 Default-on sync can post third-party ids to a chat others read, and the note was false (major). Accepted, with the public-chat refusal made a choice.** I read P21's option text (playlist-product-choices.md) and confirmed `channels.searchPosts` in the docs.
  - The first publish into a chat with members now waits for a playlist-specific confirmation.
  - The first-run bulletin appears where the first playlist is created.
  - The note depends on the sync chat.
  - Public chats are refused by default. That departs from P9's "allow sync chats that have other readers", so it is choice P28 rather than a fixed rule.
- **2.2 A forced logout kept plaintext files (major). Accepted.** Verified at `main/main_account.cpp:539-550` (upstream wipes on a forced logout). The default is now deletion, and the choice remains as P27, whose default flipped. The next-login notice is dropped.
- **2.3 Changing the sync chat left records behind and dropped the ids needed to remove them (major). Accepted.** Section 8.13 now runs a last step in the old chat, publishes into the new one, deletes the covered old records, and only then clears the memo. The chat-change confirmation wording is passed to the sync-channel design. S8 and H3 cover it.
- **2.4 Deltas were unrequested machinery (major). Accepted.** I checked the decisions (P15 asked for suggestions and whether every sync merges) and the prefer-simplest-mechanism memory. v1 publishes snapshots only, with the memo, and format 1 still accepts partial playlists. R1, C17's delta builder and C18's triggers are gone. Offered to the owner as P30.
- **2.5 The share file tells more than the confirmation says (minor). Accepted.** Merged with 1.19. The playlist id's linking role is also stated (section 10.2).
- **2.6 Paid-message chats (minor). Accepted.** Verified:
  - `api.tl:2471` (`allow_paid_stars`);
  - `data_peer.h:337-338`;
  - `share_box.cpp:1971`;
  - `apiwrap.cpp:525`;
  - Android `MessagesController.java:7316`.

  The chooser refuses such chats, and a paid-message error stops a job. SH8 covers it.
- **2.7 The caption title (minor). Accepted.** Duplicate of 1.8.
- **2.8 "Without your name" was wrong (minor). Accepted.** Checked against the forwardMessages docs. The wording is now "from you, without Forwarded from headers or captions".
- **2.9 The follow switch and Unlink (minor). Accepted.** Both are dropped, along with the imported-playlists list and SH7's old case. The preview on every file is the control. CopyPlaylist remains for Keep a copy and Import as new.
- **2.10 Toggles in synced settings.toml reach further than they say (minor). Accepted.** The wording now names the scope (section 8.9). The question about user-id keys is passed to the account-settings design as an open question. It is a hypothesis until that design exists.
- **2.11 Keep downloaded meant different things on each client, and desktop kept files are plain (minor). Accepted.** Verified that the cache key is used at `storage_account.cpp:1831-1838`. Kept now means "fetched by Download all" on both clients, and plain kept audio is disclosed (sections 9, 11 and 15).
- **2.12 Logout with sync on but no sync chat (minor). Accepted.** That case now has its own dialog, and so does held publishing (section 8.11). S7 covers it.
- **2.13 The tag carried standing conditions (minor). Accepted.** The recommendation is now `#purplechatsync` (P22). H13 now checks exact matching.
- **2.14 M1 departed from P5 with the departure preselected (minor). Accepted.** P24 now says it is a departure, and names what a bar button would take: a member, its construction and wiring, geometry, visibility and counter lines in `history_view_top_bar_widget.{h,cpp}` (verified lines in section 6.1), a new selection count, and the same in the shared-media top bar.
- **2.15 `check_minutes` and the largest-stamp guard had no decision behind them (minor). Accepted.** The interval is fixed at 15 minutes. The guard is dropped because a slow clock only delays a purge, and C20 loses that case.
- **2.16 No logging rule (minor). Accepted.** Existing practice was checked at `purple_sync.cpp:326-328`. The rule is in section 9, and X6 scans logs. S2 counts requests with Purple's own counters, not from content logs.

## 21. Product choices still open

P1 to P21 are settled: as answered in rounds 1 and 2, or at their defaults. These are the new ones this revision raises. Each lists its options and the default this design builds.

- **P22 Marker tag** (shared with the sync-channel design): `#purplechatsync` (default), `#purplesyncchat` (only while the marker stays text and every predicate matches whole tags), or another letters-only tag that begins with neither `#purplesync` nor `#purpleplaylists`.
- **P23 Re-sharing to a chat this device shared to before:** send only the new songs, with "Send all again" in the dialog (default), or always send every song.
- **P24 Desktop multi-select entry (a departure from P5's "bars"):** the selection context menus in chats, sections and shared media, with one-line hooks (default), or also a button in the selection bars, which needs about fifteen lines across the top-bar widget, the selection state and the shared-media top bar.
- **P25 Android top-level entry**, since Android has no drawer: a row in the chat list's main menu next to Saved Messages (default), a row on the Profile tab, or a row in the main Settings list.
- **P26 Keep downloaded scope:** one option for all playlists, on (default), or a switch per playlist.
- **P27 Forced logout with unsynced edits:** delete the files as upstream wipes the account (default), or keep them and offer Sync or Delete at the next login.
- **P28 Playlist records in a sync chat with other readers:** confirm before the first publish when the chat has members, and refuse public chats (default); confirm for public chats too; or rely only on the sync-chat layer's warnings, as revision 2 did.
- **P29 Record size caps:** 10 MiB compressed, 32 MiB library, 24 MiB for local edits, with in-memory downloads on both clients (default); or 32 MiB, 64 MiB and 48 MiB, which needs a file-backed download on desktop.
- **P30 Sync record shape in v1:** full snapshots only, with the memo (default); or deltas with compaction triggers, as in revision 2.

## Consistency amendments

## Consistency amendments (2026-09-30)

These come from the consistency pass against the sync chat design (revision 2) and the per-account settings design (revision 2). It was read-only. Labels are as elsewhere in this design.

1. **The tag.** `#purplechatsync` is agreed with the sync chat design (its C1) and replaces the account design's suggestion (its A9). It is one choice for the owner (X1). C13's inputs are unchanged. The word is "mark".

2. **The record predicate (8.2).** `IsPlaylistRecordCandidate(meta, facts)` is the current name check AND the sync chat design's `IsOwnSyncChatMessage(facts)`. That adds three tests:
   - in groups, `from_id` must be the user, so posts by an anonymous admin or under another send-as identity fail;
   - scheduled and TTL messages fail;
   - private chats are judged by `out`.

   The facts come from the same client mapper as settings records. H11 is the sync chat design's S3, and H12 its S9.

3. **The sync switch and options (8.4, 8.9, 8.10).**
   - `sync_p` leaves settings.toml. The switch is the key `sync` in the local sync file, per account per device, default on (P21), as in playlist-design.md (docs: lines 474-481). Its wording is "Turns playlist sync off for <account> on this device."
   - Why: a button that writes an account section puts that account's user id into the file the home account posts, and starts a settings review on every home-account device (account design, critique 2.1). The consent it pairs with is per device too.
   - settings.toml keeps `[playlists] auto_sync_p` (per account), `menu_item_p` and `keep_downloaded_p` (install-only), all default true.
   - The Playlists screen's Automatic sync toggle edits the active account's section only when that section already sets the key. Otherwise it edits the global key, and it never creates a section. Its subtitle is "For all accounts on every device that syncs settings" or "For <account> only (set in Settings > Purple)".
   - The question at the end of 8.9 is answered: sections are keyed by user id (account A1), and both settings disclosures gain a sentence saying so.

   This is merged choice X5.

4. **The scheduler (8.7).** A per-process `PlaylistScheduler` iterates every signed-in account, so accounts that are not on screen still get the app-start check.
   - It is started by the session-start call: on desktop window/window_session_controller.cpp:1596, which becomes `Purple::StartSession`; on Android ui/DialogsActivity.java:2874, which the refactor moves into `PurpleDialogsHooks`.
   - The APIs exist (verified: `Main::Domain::accounts()` at main/main_domain.h:52; `Account::sessionExists()` at main/main_account.h:75; Android `UserConfig.isValidAccount` at messenger/UserConfig.java:233).
   - Each check first calls the sync chat layer's `EnsureResolved(account)`, and one 15-minute gate per account covers both.
   - It also registers the session-end observers of 8.11.

   New hypothesis H29, checked by source reading at implementation: the scheduler can send requests for, step and upload from an account that is not active on desktop, or not selected on Android, without that account's UI.

5. **Moving between chats (8.13 replaced).** The move is the playlist part of the sync chat design's Carry. The sync chat layer runs it before posting the mark in N (switch) or before committing (follow, fill), not after a change notice. Core `PlanPlaylistMove`, in `purple_playlists_sync`, takes the local library and the present records of O and N with their statuses. It:
   - merges every valid record of both chats into the local library;
   - publishes into N when no present valid record in N equals the merged library (records in O do not count);
   - prunes in N only on a covering record read in N;
   - prunes in O on a covering record read in either chat, including in Saved Messages, because no shipped build reads playlist records;
   - forwards newer-format and oversized records into N unparsed, as newer-major settings records are;
   - leaves invalid records, which the user's old-chat option removes.

   Another device learns of a move from the sync chat layer's resolution (when a chat list opens, and at the P4 checks), not from settings sync. A's memo and cache are cleared after the layer's commit. The combined simulation test, `tests/test_sync_chat_carry.cpp`, lands with A3. C-4 and E-4 become "Move playlist records with the sync chat".

6. **Consent (8.10, P28).** The playlist confirmation is dropped.
   - The sync chat design's reader warning, which already names playlist content, is the consent. It is shown again, with only the playlist line, when playlists first post into a chat whose earlier warning did not name them.
   - The local sync file loses its list of confirmed chats, and the "waiting for your confirmation" status maps to the layer's Paused.
   - Public chats leave this design: the layer refuses them, and pauses posting if a chat turns public.
   - Readers are none, members with a count, or a named person or bot.

   P28's public refusal is settled by agreement. The rest is merged into X2.

7. **First run and status (8.8, 8.10).** If the account is Unset when its first playlist is created, the sync chat design's setup prompt opens instead of the bulletin. Otherwise the bulletin shows as written. Status strings use the shared vocabulary (sync chat amendment 11):
   - "No sync chat yet" becomes "Playlists aren't syncing yet · Set up";
   - the entry for a public sync chat is removed;
   - Searching, Lost, Paused and NewerMark are added.

   The Playlists screen shows "Sync chat: <chat> · Change", the new-reader line and the stale-marks row for the active account.

8. **Before posting, and request counts (8.4, S2, X10).** Step 7 calls the layer's `CheckBeforePost(account)`, which is R3 plus one full-info request. When the check pauses, the step skips publishing and shows the layer's reason. S2 and X10 now read:
   - a check with nothing new sends one playlist search, plus the layer's resolution (R1, usually one page, and R3), under one 15-minute gate per account;
   - a publish adds R3 and one full-info request;
   - Purple's own counters do the counting.

9. **Sending (8.2, C-2, E-2).** Records use the Purple upload that the sync chat design builds in commit 2 of its phases 2 and 3 (X3 (a)). C-2 and E-2 become "reuse", and build the upload only if the owner picks X3 (b).

10. **Logout (8.11).** The hold first calls `EnsureResolved` with a bounded wait. If the result is not Use, it shows the no-sync-chat dialog, whose Set up sync opens the layer's setup prompt. The session-end observer also deletes the layer's cache.

11. **Protected songs (3.3, 10.5).** `protectedContentAllowed` stays false until the owner settles X6. The dialog is asked once per share action, with no "don't ask again". Its final text:

   > 7 songs are from protected chats. Their owners turned off forwarding and saving, so Telegram won't forward them. Purple can upload copies from this device instead: this downloads 18.4 MB for songs not yet on this device and uploads 41.2 MB. The copies are not protected: anyone in <chat> can save and forward them. [Upload copies] [Skip these songs] [Cancel]

12. **Private listening (P20) moves into this design.** The user-control design stopped (docs: checkpoint 20:10), so P20, which the owner decided in round 1, now lives here.
   - The key is `[music] private_listening_p`, default true, per account by the account design's rule.
   - The two hooks already in section 13 become this design's: the desktop early return in `MusicListenTracker::report`, and the Android guard on the send line at MediaController.java:7002.
   - Phases B and D each gain a commit, "Keep music listening private".
   - The disclosure's conditional sentence reads the key. H4 stays.

13. **settings.toml readers.** Section 12 gains `purple/purple_settings.{h,cpp}`. A1 adds the `[playlists]` and `[music]` readers with their defaults, their entries in the account registry once it exists, and docs in docs/purple/config.md. The options are written with the account design's scoped `SetTableBool`.

14. **The chat token (3.1).** The grammar moves unchanged into the shared `purple/purple_peer_token.{h,cpp}`. That file is the first core commit of whichever of this design's phase A and the sync chat design's phase 1 starts first, and A1 uses it.

15. **Dependencies (header).** The sync chat design is now written. Its interface is:
   - `EnsureResolved` and `CheckBeforePost`;
   - the status and the readers;
   - the Carry hook;
   - the setup prompt and the chooser;
   - the session-start call.

   The user-control design did not complete. P13 waits on X6, and P20 moved here.

16. **Order.**
   - Phases A, B and D may land before the sync chat design's client phases. C and E land only after them.
   - G1/G2 and `PurpleSyncAutoDownload` still gain `Purple playlists.json.gz` and `*.purpleplaylist` in C-1 and E-1.
   - H25 (S1), H13 (S2), H11 (S3), H12 (S9), H8 (S5), H10 (with S10 and S11), H3, H26 and H27 run in the merged spike before the sync chat core. The other hypotheses stay before phases C and E.

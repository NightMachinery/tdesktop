# Purple playlists: design, revision 4

Written 2026-11-03, read-only. This revises revision 3 (2026-09-30) against today's code. It stands alone. Everything in revision 3 that still holds is restated here. What changed since then, mostly outside playlists, is absorbed: settings sync shipped, the sync chat became the Magic Purple Sync group, the upstream merge to 7.3.3 moved code, and media work started on the same player.

Trees checked:
- **tdesktop** `dev` at `38857d136c`, upstream 7.3.3 (`core/version.h:26`) with Purple on top. The branch this document is committed on adds only docs on top of it. Also read: `media` at `884225999c` and `recording` at `8f792fae58`. The tdesktop `sync-magic` branch is not public and was not read.
- **purple-core**: the gitlink this branch pins, `d51512e`, which is the tip of core `master`. Also read: `sync-magic` at `bf5da4c` (eleven commits on top of `d51512e`), `sync-magic-delete` at `7905290` (one commit on top of `sync-magic`), `media` at `6d0bd81` and `recording` at `a6631f8`.
- **purple-telegram-android** `master` at `35a08c479`, upstream 13.1.1 (`gradle.properties:17`). Also read: `sync-magic` at `0f0ef0701` (thirteen commits on top of master), `media` at `39ad300e1` and `recording` at `f1eef62b5`.

How claims are labelled:
- **Verified**: read in source during this pass, with file and line at the tree named above.
- **Docs**: read in Telegram's documentation (core.telegram.org), in the repositories' docs, or in the owner's answer files. Telegram documentation quoted here was read for revision 3 on 2026-09-30 and was not re-read.
- **Measured**: from revision 3's scratch scripts on synthetic data (no build). Nothing was re-measured.
- **H1 to H30**: hypotheses for the disposable-account spike (section 19).

Paths: desktop paths are relative to `Telegram/SourceFiles/` unless written in full. Android paths are relative to `TMessagesProj/src/main/java/org/telegram/` unless written in full. Core paths are relative to the purple-core root.

## What changed from revision 3

**The sync chat**
- **Playlist records go into the account's sync chat as the sync layer binds it.** That is the Magic Purple Sync group by default, or Saved Messages when the person bound it by hand. They share the chat with settings records and are told apart by file name, caption tag and an ownership check (section 8.2).
- **The marker tag is gone** (P22 retired). There is no mark, no global search and no redirect. The layer finds its chat in the local chat list (section 8.3). H25 is retired, and H13 shrinks to the Saved Messages case.
- **Sync chats with other readers are gone** (P28 retired). The magic group must have the account as creator, sole member and sole admin, and a chat picked by hand must have no other member or admin either. So the playlist-specific confirmation, the "other readers" note, the public-chat refusal and the per-chat confirmation list in the local sync file are all dropped. This also overrides the owner's round 2 answer to P9 (section 21).
- **Discovery in the magic group reads its history** instead of searching it. The group holds a handful of Purple messages, so one `messages.getHistory` page usually lists every record, with no search-index lag (section 8.3). Saved Messages keeps revision 3's tag search.
- **Moving between chats is simpler.** The carry journal that revision 3's amendments relied on is deleted on core `sync-magic-delete`. When the binding changes, playlists publish a snapshot into the new chat from the local library and delete their own covered records from the old chat, best effort (section 8.12).
- **Who creates the group.** The layer creates it when the local list and one `messages.getAllChats` reply prove it absent. Default-on playlist sync (P21) may therefore create a group on the account. This is new choice P33.

**Settings, user control and dependencies**
- **Protected content (P13) shipped as an always-on policy with no setting**, on both clients (section 3.3). So `protectedContentAllowed` reads true from the start, protected songs can be added, and sharing can upload copies of them once the upload supports big files. Nothing waits for a user-control layer any more. The missing switch is choice P34.
- **Private listening (P20) does not exist in either client.** Playlists build it, as revision 3's amendment 12 said, and this revision proposes landing it first as a small standalone change (choice P35).
- **Per-account settings (P10) shipped as per-account files layered over the global file**, not as sections inside one `settings.toml`. Playlist options read through that layering (section 8.8).
- **A Purple-owned upload exists on both clients**, for settings records: desktop `SyncConfigUpload` and Android `PurpleSyncChatUpload`. Playlists generalize it. They do not build a second one (section 8.2).
- **Local files move into the account directory** that core's `ResolveAccountFiles` names, beside that account's `sync/` folder (section 7.1).

**Code moved**
- **Every file:line is re-verified** at the trees above. Most hook sites moved by tens to hundreds of lines. Android's chat menu, file tap and player-sheet hooks now go inside Purple classes that already exist (`PurpleChatHooks`, `PurpleMediaSave`), so they cost no new upstream line (section 13).
- **Upstream 7.3.3 added per-item track lists to the desktop player** (`moveInItem`, `trackInItem`). The playlist hooks still go first in each function (section 5.2).
- **New repository rules**: Purple sources are registered in `Telegram/cmake/purple.cmake`, strings go in `Telegram/Resources/langs/purple.strings`, and each upstream hook updates `docs/purple/upstream_hooks.md` (tdesktop `AGENTS.md`, "Keeping upstream merges easy"). Revision 3 put strings in literals and sources in `Telegram/CMakeLists.txt`. Both are corrected.

**Media work**
- **The media and recording branches touch the same player.** Desktop F3 adds a Voice-only hook inside `moveInPlaylist`, and F9 replays an ended track from Purple code. None of it conflicts with playlist mode, but three rules keep it that way (section 5.5).

**Plan**
- Phases are regrouped around what exists now. Core phase A can start today. Desktop and Android sync (phases C and E) wait for each client's sync-magic work to reach its main branch. Effort estimates are added (section 15).

## Where the brief for this revision was wrong or incomplete

Each point the brief listed was checked in code. These parts did not match:
- **"purple-core branch sync-magic and its docs/sync-chat-core.md".** `docs/sync-chat-core.md` is the mark-based contract and exists on core master already. The magic policy is described in `docs/sync-chat-magic.md`, which `sync-magic` adds. On `sync-magic-delete`, `sync-chat-core.md` is trimmed to peer tokens, ownership and eligibility.
- **"the gitlink this branch pins".** It pins `d51512e`, core master, which predates the magic policy. `purple_sync_chat_magic.{h,cpp}` exists only on `sync-magic` and its child. Playlists' core work must base on those branches, or wait for them to merge.
- **"archived and muted".** True on Android `sync-magic`, but both are cosmetic steps: a failure is logged and the lookup goes on (`messenger/purple/PurpleSyncMagicChat.java:398-399`, `:421`). Playlists must not assume the group is muted. That is one more reason every playlist post is `silent`.
- **"hidden in Purple".** Hidden unless the person turns on "Show sync chat in chat list", which sets `listed` in the binding (core `docs/sync-chat-magic.md`, Binding). A chat the person picked by hand is never hidden.
- **"records are accepted only if sent by the account itself, not forwarded and not via a bot".** The shared ownership predicate is stricter. It also refuses scheduled, TTL, non-regular and send-as messages, and in groups it requires `out` and a sender equal to the account. An unknown fact gives Unknown, not Other (core `docs/sync-chat-core.md`, "Marks and ownership"). Playlists use it unchanged.
- **"Settings sync shipped".** On desktop `dev`, manual settings sync and automatic settings sync shipped for Saved Messages only. Every sync-chat change switch is off (`docs/purple/sync.md`, "Changing the sync chat" and "Automatic settings sync on Desktop"). The magic group is live only on Android `sync-magic`, which is not merged into Android master. Desktop's magic work could not be read.
- **"the user-control layer (protectedContentAllowed, private listening)".** There is no such layer and no such flag. Protected content shipped as constants in each client's copy and save policy, and private listening was not built (section 14).

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
  - asks the sync layer which chat to use;
  - lists the playlist records in that chat (one history page in the magic group);
  - downloads only records this device has not merged before;
  - merges them;
  - runs the automatic purge when it is safe to;
  - publishes a snapshot of the whole library if no record already equals it;
  - deletes records that another record covers.

  The steady state is one record, and one request per check.
- **The sync chat** is the Magic Purple Sync group: a private megagroup that only the account is in, found in the local chat list and created if absent, hidden in Purple. Settings records and playlist records share it.
- **Deleting** has two levels. Delete puts a playlist in Recently deleted, and Delete forever purges it. A playlist left in Recently deleted for 30 days is purged automatically.
- **Sharing** forwards the songs to a chat, then sends a share file.
  - The songs arrive from the sharer, with no "Forwarded from" header and no caption.
  - A Purple recipient opens the file as a playlist, and a later file merges into it after a preview.
  - Songs from protected chats are re-uploaded after a confirmation.
- **Offline listening:** **Download all** per playlist. With **Keep downloaded** on, which is the default, those files are kept past Keep Media and the cache limits.
- **Playback** uses Telegram's own players in playlist order, through guarded hooks.
- **Privacy.**
  - Private listening is the default.
  - Records never hold access hashes, file references or the account's own id.
  - Share files never name the sharer's source chats or message ids.
  - Records go only to a chat nobody else can read.

## 1. Terms

- **Library**: all playlists of one Telegram account on one device, kept as one canonical JSON document.
- **Sync layer**: the per-account code that binds the sync chat. Its policy is core `purple_sync_chat_magic` (on `sync-magic`); each client runs the lookup.
- **Sync chat**: the chat the sync layer has bound for an account. In **magic** mode it is the Magic Purple Sync group. In **manual** mode it is Saved Messages, or another chat the account picked that has no other member or admin.
- **Magic group**: a private megagroup titled exactly `Magic Purple Sync` (`kMagicSyncChatTitle`, core `purple/purple_sync_chat_magic.h:25` on `sync-magic`), with the account as creator, sole member and sole admin, and no username.
- **Binding**: the layer's per-device file `sync/chat.json` for one account: mode, bound chat, waits, problem, `listed` and rejected candidates.
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
- **Present records**: the records the current step knows to exist in the sync chat: those its listing returned, plus memo entries the listing missed but a fetch by id confirmed.
- **Memo**: the list of records this device has already classified in one sync chat. Each entry is keyed by message id and document id. It holds the record's status, the version of the classifier, and for valid records the hash of the canonical bytes and a cached copy of the compressed bytes.
- **Playlist step**: one sync pass: resolve the chat, list, read the new records, merge, purge, publish and prune (section 8.5).
- **Share file**: a small gzip file sent after the forwarded songs. It holds one playlist without any source references (section 10.6).
- **Exchanged set**: for one playlist on one device, the document ids this device has sent in a share of it, or adopted from a share file of it. It is local and never synced (section 10.6).
- **Copy map**: on one device, the map from a protected original's document id to the document id of the copy that device uploaded, for each destination chat. It is local.
- **Playlist mode**: the player is stepping through a Purple playlist, not through the playing song's chat.
- **Local sync file**: a small per-account file on the device. It holds status, the memo and bookkeeping (section 7.1), and is never posted.

## 2. What a playlist is and holds

Unchanged from revision 3.

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

**The grammar already exists in core.** `purple/purple_peer_token.{h,cpp}` (core master, verified) defines exactly these four forms, refuses zero, signs, leading zeroes and overflow, and normalizes `user:<self id>` to `self` when the caller supplies the account's id (core `docs/sync-chat-core.md`, "Peer tokens"). Revision 3's amendment 14 planned to create that file. Playlists now reuse it, and always pass the account's id, so `self` is written for Saved Messages and the account's id never is.

**Why the type is required.** Docs: peer id sequences overlap across types, and basic groups and supergroups number their messages separately. Purple's `ChatKind::Group` merges those two kinds, so it cannot be reused here.

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
  - On desktop that is `DocumentData::isSong()` (verified, `data/data_document.h:201`).
  - On Android the helper checks the attributes directly. That set is inside `MessageObject.isMusic()`.
- **The message is a regular server message.** That rules out scheduled messages, unsent local messages, sponsored messages, stories, upstream Profile music items and secret chats. On desktop, upstream 7.3.3 calls Profile music items and scheduled messages "real playback contexts" (`IsRealPlaybackContext`, verified, `media/player/media_player_instance.cpp:147-151`); playlists still refuse both.
- **The message's main media is that document**, not a poll's, a web page's, or one of several tracks inside a rich message. Upstream 7.3.3 lets the desktop player step through the audio of one rich message (`ItemRichPageAudio`, verified, `media/player/media_player_instance.cpp:153-163`); such tracks share one message id, so v1 leaves them out.
- **The message has no self-destruct timer.**
- **Protection (P13).** Songs from protected chats, and protected messages, qualify while the client's save policy is unrestricted. Today it always is (below).

**Core holds this rule** as a pure function, `QualifySong`. It works over plain fields the client fills in, plus the protected-content flag, so both clients decide the same way. The flag is named `protectedContentAllowed` in core. Each client fills it from its existing policy function, not from a new setting:
- desktop: `Purple::SaveRestrictionsDisabled()`, an inline function that returns true (verified, `purple/purple_user_control.h:36`);
- Android: `!PurpleContentPolicy.restrictSaving(true)`, where `restrictSaving` returns false (verified, `messenger/purple/PurpleContentPolicy.java`).

If the owner later turns P13 into a switch (choice P34), playlists follow it with no change. Songs already in a playlist stay and keep playing when the flag turns false. Only new adds and new copies for sharing are refused.

**Where P13 meets upstream's protection gates.** Revision 3 listed the gates that playlists sit behind and asked the user-control layer to lift them at the root. Most are lifted now, by Purple policy calls already in upstream files:
- **Desktop chat menu.** `AddSaveDocumentAction` is called under `!hasCopyMediaRestriction(item) && (SaveRestrictionsDisabled() || !ItemHasTtl(item))` (verified, `history/history_inner_widget.cpp:3188-3201`). `hasCopyRestriction` returns false while `CopyRestrictionsDisabled()` (verified, `:3993-3998`; `purple/purple_copy.h:15` returns true), and `forbidsSaving` returns false while `SaveRestrictionsDisabled()` (verified, `history/history_item.cpp:3446-3449`). So the gate is open in protected chats.
- **Desktop section lists.** The second `AddSaveDocumentAction` overload returns early on `list->hasCopyMediaRestriction(item)` (verified, `history/view/media/history_view_save_document_action.cpp:121-129`), which returns false under the copy policy (verified, `history/view/history_view_list_widget.cpp:2131-2137`). Open.
- **Desktop multi-select.** `Menu::AddDownloadFilesAction` is called under `!hasCopyRestrictionForSelected()` (verified, `history/history_inner_widget.cpp:3504-3509`, `:3811-3816`; `history/view/history_view_context_menu.cpp:1219-1232`), which returns false under the copy policy (verified, `history/history_inner_widget.cpp:4029-4031`, `history/view/history_view_list_widget.cpp:2171-2174`). Open.
- **Desktop shared media, single song: still closed.** The list calls `AddSaveDocumentAction` only when `_provider->allowSaveFileAs(item, document)` (verified, `info/media/info_media_list_widget.cpp:1556-1561`), and the shared-media provider returns `item->allowsForward()` with no Purple policy call (verified, `info/media/info_media_provider.cpp:587-591`). So in a protected chat's shared media, and in the player's hover list, which is built from it, a single song gets no Save item and no Add to playlist. The fix belongs with the save policy, not with playlists: one guarded condition in `Provider::allowSaveFileAs`, `|| Purple::SaveRestrictionsDisabled()`, as the same file already does in `hasSelectRestriction` (verified, `:85`). It is listed as commit T1 in section 15. Until it lands, Add to playlist is missing there and present everywhere else.
- **Android player sheet.** Upstream still adds "Save To" only when `!noforwards` and skips it for the Profile music list (verified, `ui/Components/AudioPlayerAlert.java:2878-2882`). But Purple's `PurpleMediaSave.addAudioOption(o, messageObject)` now runs at the top of both sheet menus (verified, `:2876` in `showMenuOptions`, `:2952` in `showOptions`), and a row's option button is no longer null for a protected song, because its condition calls `PurpleContentPolicy.restrictSaving(noforwards)` (verified, `:2539`). Playlists add their rows inside `PurpleMediaSave.addAudioOption`, with no upstream line.
- **Android chat menu and action bars.** Purple adds its own rows there, and `QualifySong` decides, so no upstream gate applies.

Timer messages stay out, because their source vanishes by design. Desktop now keeps save-only copies of some timer media (`docs/purple/user_control.md`), but only for voice, round video, photos and videos. Referencing kept copies, and songs kept by the deleted-message archive, is a follow-up (section 15, Later).

### 3.4 Resolving on a device

Unchanged from revision 3, with lines updated. Resolution runs when a playlist opens (visible rows first, in batches, then the rest in the background), before Play on Android, ahead of the playing song on desktop, and before a share. Results are kept in memory only.

1. **Local first.** Desktop uses `Data::Session::message(peerId, msgId)`. Android uses `MessagesStorage.getMessage(dialogId, msgId)`.
2. **`self`, `user` and `chat` sources.** Request `messages.getMessages` with bare message ids (schema verified, `mtproto/scheme/api.tl:2593`), in batches: on desktop through `ApiWrap::requestMessageData` (verified, `apiwrap.cpp:644`), on Android in batches of up to 100. This needs no peer access hash (H1).
3. **`channel` sources.** Request `channels.getMessages` (schema verified, `api.tl:2903`), which needs the channel's access hash on this device:
   - if the channel is known, request it directly;
   - if the channel is unknown and `un` is present, resolve the username first;
   - otherwise show "Open the channel once on this device", and retry when the dialog list finishes loading (H2).
4. **Check the document.** The fetched message must carry this entry's document id. Otherwise the source is "Changed".
5. **Fallbacks (P11).** If the primary source ends in Deleted, No access, Changed or Restricted here, try each `alt` source in order with the same steps. The first that resolves is used for playback and sharing.

Once the message is loaded, file-reference refresh works exactly as it does from a chat: desktop's `ApiWrap::refreshFileReference` (verified, `apiwrap.cpp:2844`, `:2855`), and Android's `FileRefController`.

**Requests go through the shared Purple RPC helpers** where a client has them: Android `PurpleRpcRequests.send`, which keeps flood waits per method across restarts (verified, `messenger/purple/PurpleSyncTelegramClient.java:137-150` on `sync-magic`). Desktop uses its `SyncRequestSender`. A resolution batch that hits `FLOOD_WAIT` waits; it never retries in a loop.

### 3.5 Unavailable songs

Unchanged. A row shows one of these states:
- **Resolving**;
- **Deleted**: the message is gone;
- **No access**: the channel was left, is private (`CHANNEL_PRIVATE`), or is unknown here;
- **Changed**: the message now carries another file;
- **Restricted here**: a platform restriction applies;
- **Couldn't load**: a network failure, retried the next time the playlist opens;
- **Playing an earlier copy**: the primary source failed and a fallback resolved. The row menu offers **Use this copy**, which relinks the song to that fallback.

Unavailable rows stay in place, greyed, with their saved title and performer and a one-line reason. Playback and sharing skip them. Nothing is removed or relinked automatically, and an unavailable song is never a sync error. To relink a song, add it again from any chat where it still exists, or pick **Use this copy**.

## 4. Creating and editing

Unchanged from revision 3.

### 4.1 Operations

**How operations work.** Every operation is a pure core function of (library, operation, time in milliseconds, entropy). It returns one of:
- the new library plus an **undo list**, which records every register the operation overwrote and its prior value;
- a typed refusal.

Each operation takes a stamp s from its playlist's clock (section 7.6). The client saves the library file, records `unsynced_since` if it is not already set, and restarts the automatic-sync timer (section 8.6).

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

**The first playlist.** When the account's first playlist is created, from this menu or from the Playlists screen, the first-run bulletin appears right there (section 8.9).

### 4.4 Limits and sizes

- **Local operations**, imports included, refuse to grow the canonical library past **24 MiB**. The refusal reads "Playlists are full. Delete forever deleted playlists, or remove songs, to make room."
- **A merge is never refused.** A merged library above the record caps can still be edited downward and synced by reading, but this device publishes no snapshot until it fits again (section 8.7).
- **The local file has no read cap**, so a library that grew past 32 MiB by merges still opens after a restart (section 7.4).

**Measured sizes** (revision 3). These are canonical compact JSON in this format, from synthetic libraries with 17% removed songs, half Persian text and half with `un`:
- 500 entries: 90,353 bytes, or 24,899 gzipped.
- 2,000 entries: 365,447 bytes, or 97,917 gzipped.
- 7,200 entries in 40 playlists: 1,323,727 bytes, or 354,207 gzipped. That is about 180 bytes per entry, and a gzip ratio of 3.6 to 4.1.
- At about 184 bytes per entry, 24 MiB holds about 137,000 entries and 32 MiB about 182,000.
- A 24 MiB library gzips to about 6 to 7 MiB, and a 32 MiB one to about 8 to 9 MiB. Both fit the 10 MiB record cap unless the text compresses worse than a ratio of about 3.2.
- A bomb: 64 MiB of one repeated byte gzips to 65,251 bytes. A size check on the compressed file alone is therefore no guard (section 7.4).

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
- **Listening reports (P20).** With `[playback] private_listening_p` true, which is the default, no song, from a playlist or a chat, sends `messages.reportMusicListen` (schema verified, `api.tl:2839`; H4). Nothing in either client does this today. The hook sites:
  - **Desktop:** `MusicListenTracker::report` (verified, `media/player/media_player_listen_tracker.cpp:67-72`). One early return goes after its three `base::take` lines, so the tracker still resets.
  - **Android:** the tracker's `report()` (verified, `messenger/MediaController.java:7527-7542`). A guard condition goes on the send line at `:7538`: `if (!PurplePrivacy.privateListening()) ConnectionsManager...sendRequest(req, null);`. The resets at `:7540-7541` then still run, so listening gathered while private listening is on is never reported later if the switch turns off in the middle of a song.

  While private listening is off, playlist playback reports exactly as chat playback does, because every song is a real, loaded message.
  The key moves from revision 3's `[music]` table to `[playback]`, which the media work already uses for `private_chat_queue_p` and the seek step (`docs/purple/media_playback.md` and `docs/purple/media_recording.md` on tdesktop `recording`). Its settings row goes in the same Media playback section.

### 5.2 Desktop

`Media::Player::Instance` has no explicit queue. All navigation goes through three members: `moveInPlaylist` (verified, `media/player/media_player_instance.cpp:611`), `nextAvailable` (`:824`) and `previousAvailable` (`:805`). OS media controls, media keys, the dock menu and the Windows taskbar call the same members. Repeat One acts before `moveInPlaylist` is reached (verified, `:1471`), and so does the `disable-autoplay-next` option (verified, `:80`).

**New in 7.3.3.** Each of the three members first asks about tracks inside the current message: `moveInPlaylist` calls `moveInItem` before anything else (verified, `:611-617`, with `moveInItem` at `:596`), and both `*Available` members call `trackInItem` first (verified, `:809`, `:828`). Playlists never queue such tracks (section 3.3), so a playlist hook placed first in each function keeps its revision 3 shape.

**The new `purple/purple_playlist_player.{h,cpp}`** keeps:
- the session and the playlist id;
- the snapshot of document ids;
- the current position and the shuffle order;
- the **started id**: the `AudioMsgId` it last started.

It plays each song with `instance()->play(AudioMsgId(document, sourceFullMsgId))`, so streaming and file-reference refresh behave as they do from a chat. It records the started id **before** calling `play()`, because `play()` can fire `playlistChanges` synchronously. If the next song is still resolving when a move arrives, the move returns true and the song starts once it resolves, **but only if the player's current track is still the started id and is not playing**. Otherwise the resolved song is dropped. If it fails, the move skips on.

**Hooks:**
- **`media/player/media_player_instance.cpp`: one include and three guarded early returns.**
  - At the top of `moveInPlaylist`, before the `moveInItem` call, for the Song queue only: `if (const auto moved = Purple::PlaylistMove(data->current, delta, autonext)) { return *moved; }`.
  - At the top of `nextAvailable` and `previousAvailable`, before `trackInItem`, for the Song type: return `Purple::PlaylistAvailable(current, +1 or -1)` when it has a value.

  Each Purple function returns `std::nullopt` unless the player's current track equals the started id. So playlist mode ends by itself the moment anything else plays, pause and resume do not end it, and the Voice queue is never touched.
- **`media/player/media_player_panel.cpp`: one include and one early return** inside the `peer` lambda of `Panel::refreshList()` (verified, `:236`, lambda at `:241-265`): `if (Purple::PlaylistClaims(current)) return nullptr;`. A null peer destroys any list already built, and the panel stays closed.
- **`media/player/media_player_widget.cpp` (P7): one include and one guarded early return inside the label branch of `Widget::mouseReleaseEvent`**, right after upstream's `if (_labelsOver != downLabels) { return; }` (verified, `:522-526`): `if (Purple::PlaylistTrackClicked(_type)) { return; }`.
  - While the playing song belongs to a playlist, a click on the track name opens that playlist's box, scrolled to the playing row.
  - A return at the top of the function could not tell a click on the track name from any other release, because `_labelsDown` is private and has to be taken first.
  - Upstream's own action in that branch, which shows the item, runs only for Voice or for a song from another session (verified, `:527-536`), so nothing upstream is lost.

**Known v1 limits:**
- Upstream still builds its 32-id window around each song's own chat.
- With Shuffle on, upstream also pages that chat's music ids to build its own shuffle list (`validateShuffleData`, verified, `:1032`). Playlist mode never uses that list. Acceptance D2 counts these requests.
- The chat does not scroll along with playlist songs. Purple's moves do not fire upstream's `_switchToNext`, which is what moves the chat and what the voice queue listens to (section 5.5).

### 5.3 Android

`MediaController` keeps one process-wide queue, and three of its behaviours break playlist order:
- It re-sorts the queue by message id on every set (verified, `sortPlaylist` at `messenger/MediaController.java:2924`, called from `setPlaylist` at `:2901`).
- A late `musicDidLoad` for the playing song's chat merges that chat's songs into the queue (verified, branch at `:1904`, sorting at `:1916`).
- `loadMusic=false` sets `forceLoopCurrentPlaylist` (verified, `:2881`), which overrides both Repeat One and stop-at-end.

No Purple hook exists in `MediaController.java` on master today (verified: no "Purple" in the file). The media branches add one, in the voice playlist only (section 5.5).

**Edits to `messenger/MediaController.java`**, each marked `// Purple:`:
- **One import.**
- **One guard condition** in the same-object shortcut at the top of the five-argument `setPlaylist` (verified, `:2873-2874`): `if (playingMessageObject == current && !PurplePlaylistPlayer.replacingQueue())`.
  - This is the rule's "smallest extension point". No hook of call or return shape can skip that shortcut, and Purple cannot reach the private queue itself.
  - The alternative is to restart the current song through `cleanupPlayer`. That needs no edit at all, but it restarts a song the person is listening to.
- **One early return** at the top of `sortPlaylist()` (`:2924`): `if (PurplePlaylistPlayer.keepsOrder(this)) return;`.
- **One early return** at the top of the `musicDidLoad` branch (`:1904`), with the same condition. Nothing follows the branch chain in `didReceivedNotification`, so returning there skips only that branch.

**Nothing else in the file changes.**
- **No field.** `PurplePlaylistPlayer` holds the state. `keepsOrder(mc)` is true while one of two things holds:
  - Purple's own `setPlaylist` call is running;
  - `mc.getPlaylist()` (public, verified, `:4429`) still holds exactly the objects Purple passed, in the same positions.

  When upstream sets another queue, the objects change and the flag lapses by itself, so `clearPlaylist()` (`:1714`) needs no hook.
- **No overload, and no edit to the forced-loop line.** Purple calls the existing five-argument overload with `loadMusic = true` and its own `PlaylistGlobalSearchParams`, whose `endReached` is `true`. The class is public (verified, `:7344`). So:
  - `forceLoopCurrentPlaylist` stays false, and Repeat One and stop-at-end work;
  - the `loadMusic` branch generates a class guid instead of loading the chat's music (verified, `:2913-2917`);
  - `loadMoreMusic()` (`:2760`) returns without a request, because `endReached` is set (verified, `:2770`).

  H24 checks the whole path.
- **The params change the player sheet, and that is accepted.** `currentPlaylistIsGlobalSearch()` (verified, `:7367`) returns true while params are set. Then:
  - the sheet keeps its "Music" title instead of the chat's name (verified, `ui/Components/AudioPlayerAlert.java:1345-1376`);
  - rows use `AudioPlayerCell.VIEW_TYPE_GLOBAL_SEARCH` (verified, `ui/Cells/AudioPlayerCell.java:86`, `:112`), which appends the song's chat after the performer.

  That suits a playlist whose songs come from many chats. Acceptance A2 checks it on screen.

**How the order comes out right.**
- `setPlaylist` adds the given list from its last element to its first (verified, `:2889`).
- With the sort skipped, the stored array is the playlist reversed.
- "Next" moves toward index 0, and the player sheet lists the queue from the last index down.

So passing the playlist in display order gives the right next and previous, and a sheet that shows song 1 on top. Shuffle uses upstream's `buildShuffledPlayList` (`:2735`).

**How a queue is built.** Upstream's queue holds only real `MessageObject`s, and auto-advance plays whichever object comes next (`playNextMessageWithoutOrder`, verified, `:2975`). So an unresolved song cannot sit in the queue.
- Resolution starts when the playlist opens: visible rows first, then the rest in the background.
- **Play** waits until every visible entry has resolved or failed, and shows "Preparing 120 of 420 · Cancel" while it waits.
- Failed songs are left out of the queue, as on desktop.
- `messenger/purple/PurplePlaylistPlayer.java` then calls `setPlaylist` once, with `replacingQueue()` true around the call.

**The song that is already playing.** When an entry is the song already playing, the player puts that exact playing object into the list instead of a fresh copy:
- `playMessage` returns early on `isSamePlayingMessage` without replacing `playingMessageObject` (verified, `:3645-3651`);
- `MessageObject` overloads only `equals(MessageObject)` (verified, `messenger/MessageObject.java:12253`), so `indexOf` compares object identity;
- `setPlaybackOrderType` (`:4290`) clears the playlist and stops playback when `indexOf(playingMessageObject)` is -1.

With the substitution and the guard condition, the song resumes instead of restarting, and turning Shuffle off keeps it playing.

Show in chat, Forward and Save To all work in the player sheet, because every item is the real message.

**Known limits:**
- The Android Auto browse tree does not list playlists.
- A killed process forgets the queue, but the playlist remains.
- `playlistMap` is keyed by message id alone. With loads off it is never used for merging, so two songs with equal ids from different chats both play (A2 checks this).
- A large, channel-heavy playlist can take a few seconds to prepare the first time.
- **A voice message, round video or video in the middle.** Upstream 13.1.1 saves the music position before playing one (`saveMusicPlaylistStateIfNeeded`, verified, `messenger/MediaController.java:3667-3668`) and resumes the song after it (`restoreMusicPlaylistState`, called at `:2482`, `:3380`, `:3901`; state class at `:7373-7389`). The queue array itself is not replaced, and the restore matches the song by dialog id and message id, so `keepsOrder` still holds and playlist mode survives. A2 checks it.

### 5.4 Pinned music, in chat order

Unchanged from revision 3. Neither pinned-music engine produces chat order today:
- **Desktop.** The job queues each song straight into `document->save` as it discovers it (verified, `purple/purple_pinned_music.cpp:169`). The order is the newest pinned anchor first, then that anchor's neighbours.
- **Android.** The job fills a `LinkedHashMap` in the same discovery order (verified, `ui/PurplePinnedMusic.java:378`). The list screen plays those rows through `setPlaylist(..., false, null)` (verified, `ui/PurplePinnedMusicList.java:303`).

**The fix, in Purple files only:**
- **Chat order** is a core sort key: songs from the migrated-from basic group first, then message id ascending.
- **Desktop.** The job gains a collect mode that records ids, applies `QualifySong` and counts skipped audio. The ids are then sorted into chat order and saved as a new playlist named "<chat> pinned". The toast reads "Saved 42 songs as “<chat> pinned” · Open", plus "3 audio files skipped (not tagged as music)" when any were skipped.
- **Android.** **Save as playlist** in `PurplePinnedMusicList` does the same. Playing the pinned list sorts the rows into chat order and passes them through the same ordered path as playlists (section 5.3), which fixes migrated groups.

### 5.5 Living with the media and recording work

The media work (F1 to F5, branch `media`) and the recording work (F6 to F11, branch `recording`) change the same players. Each feature was checked against playlist mode.

**Desktop.**
- **F3, the private-chat voice queue.** It adds one guarded call inside `moveInPlaylist`, after the new index is computed: `Purple::ConfinedVoiceNext(data->type, ...)` (verified, `media/player/media_player_instance.cpp`, diff `dev...recording`, about `:707`). It acts only for the Voice type, and it listens to `switchToNextEvents` and Voice `trackChanged` (`docs/purple/media_playback.md` on `media`). The playlist hook is the first statement of the same function and acts only for Song. They do not interact, but both edit one function, so whichever lands second rebases onto the other and keeps the playlist line first.
- **Voice pauses a song.** Upstream pauses the song for a voice or round message and resumes it after (`pauseSongForVoice` and `resumeSongAfterVoice`, verified, `:1221`, `:1235`). The Song track stays the started id, so playlist mode survives a voice message. D2 checks it with F3 on.
- **F9, seek back and forward.** It seeks the active track and, after a track ends, replays its last seconds through `Instance::play` while that track is still the player's current one (`docs/purple/media_recording.md`, "Rules"). In playlist mode the current track after an end is either the next song (the playlist already moved) or, at the end of the queue, the ended song. Replaying the ended song keeps the started id, so playlist mode continues. The one race is a next song still resolving when the person presses back: the rule in section 5.2 (start the resolved song only if the started id is current and not playing) drops the pending song instead of cutting into the replay. D2 gains that case.
- **F9 widget hooks.** It adds three lines to `media_player_widget.cpp` (constructor, geometry and time width; diff `dev...recording`), none in `mouseReleaseEvent`. No conflict with the P7 hook.
- **F1, F2, F4, F5** touch the floating round and voice or round double-clicks (`media/player/media_player_float.cpp`, history widgets). Songs are not involved.

**Android.**
- **F3** adds one line in the `didReceiveNewMessages` branch of `MediaController`, for `voiceMessagesPlaylist` only (verified, `messenger/MediaController.java:1975` on `recording`). The music queue is a separate list. No conflict with the four playlist edits, which sit in the `musicDidLoad` branch, `setPlaylist` and `sortPlaylist`.
- **F9** adds rewind and fast-forward to `MusicPlayerService` (verified, diff `master...recording`). Next and previous in the notification remain upstream's, which follow the ordered queue (H5).
- **F7 and F8** keep recordings alive in the background and across chats. Recording takes audio focus and pauses music; the queue stays, so playlist mode survives (A2 checks that Play resumes in order after a recording).

**Three rules for both efforts.**
1. Playlist hooks act only for the Song type (desktop) or the music queue (Android). Media hooks act only for Voice. Neither reads the other's state.
2. Playlist mode is defined only by "the current track is the one Purple started", so any media feature that plays something else ends it cleanly, and any feature that replays the current track keeps it.
3. Shared settings rows: private listening (P20) joins the Media playback section and the `[playback]` table that F3 and F9 already use.

## 6. UI entry points

### 6.1 Desktop

**Add to playlist.** One call at the top of the first `HistoryView::AddSaveDocumentAction` overload, after Purple's existing TTL early return (verified, `history/view/media/history_view_save_document_action.cpp:37-45`): `Purple::AddPlaylistActions(addAction, item, document, controller);`.
- It inherits the gates of the overload's three callers, listed in section 3.3: the main chat, the section lists, and shared media, which includes the player's hover list.
- It adds:
  - **Add to playlist**: a submenu that behaves as in section 4.3, with **New playlist...** at the bottom;
  - **Add album to playlist**, for a song in an album of two or more songs;
  - **Open as playlist**, for a share file (section 10.7).
- Code: `purple/purple_playlists_menu.{h,cpp}`.
- **A known limit, accepted.** While a song is downloading, the shared-media menu offers only Cancel (verified, `info/media/info_media_list_widget.cpp:1533-1539`), and the chat menu does the same. D1 records whether the playing song reports `loading()`.

**Multi-select (P5 c, and choice P24).** The owner asked for the entry in the multi-select bars. Desktop's selection bar still has no place that a hook of the rule's shape can fill: `TopBarWidget` holds its selection buttons as members (verified, `history/view/history_view_top_bar_widget.h:237`), each constructed, wired and laid out in several places. So by default the entry goes in the selection context menus, which is a departure from the owner's answer and stays choice P24:
- one guarded call at the top of each `Menu::AddDownloadFilesAction` overload (verified, `menu/menu_item_download_files.cpp:237`, `:266`): `Purple::AddSelectedToPlaylistAction(menu, window, items);`. These overloads are called from the chat's selection menu (verified, `history/history_inner_widget.cpp:3505`, `:3812`) and from the section lists (verified, `history/view/history_view_context_menu.cpp:1219-1232`, `:1415`);
- one delegating call in shared media's selection menu, before **Clear selection** (verified, `info/media/info_media_list_widget.cpp:1618-1623`).

The item reads **Add N songs to playlist...**. It appears when the selection holds at least one qualifying song. Other selected items are skipped, and the toast says how many. It opens the same picker as a single song, and adds the songs in chat order.

**Top-level entry (P8 b).** One delegating menu row in `window/window_main_menu.cpp`, next to the existing Purple preset entry (verified, `:772-777`). It is shown while `[playlists] menu_item_p` is true, which is the default. It opens `Purple::PlaylistsBox`, which shows:
- playlists, recently used first, each with its song count and a marker on the one that is playing;
- **New playlist**;
- **Recently deleted**, shown when any deleted playlist exists, with "Deletes forever on <date>", **Restore** and **Delete forever**;
- the sync line (section 8.9);
- the note: "Profile music is Telegram's public list on your profile. Playlists here are not shown to anyone else unless you share them." With the magic group or Saved Messages this is always true, so revision 3's variant for chats with other readers is gone.

**PlaylistBox**, for one playlist:
- **Play** and **Shuffle**, and **Download all** (section 11);
- rows showing title, performer, duration, state and a downloaded mark, where a click plays from that row;
- a row menu with **Remove** (with an Undo toast), **Show in chat**, **Use this copy** (only on rows playing an earlier copy) and **Add to playlist...**;
- a box menu with **Share...** (section 10), **Rename** and **Delete** (with an Undo toast);
- drag to reorder, on **one painted widget** that paints only the visible range. `Ui::VerticalLayoutReorder` needs one widget per row, which does not scale.

Code: `purple/purple_playlists_box.{h,cpp}`. A box avoids a new `Info::Key` variant.

**Pinned music:** **Save as playlist** sits beside Download (section 5.4).

**Opening a share file.** Two guarded early returns:
- **An undownloaded file.** A tap on it reaches the save handler, not the open handler. The document view sets `result.link = _openl` only when the file is loaded or playable, and `_savel` otherwise (verified, `history/view/media/history_view_document.cpp:1486-1491`). The save handler would then write the file to Downloads or open a save dialog (verified, `data/data_file_click_handler.cpp:74`). So one early return goes at the top of `DocumentSaveClickHandler::onClickImpl` (verified, `:143-145`, now a single `SaveAndTrack` call): `if (Purple::OpenPlaylistShareOnTap(document(), context())) { return; }`.
  - It acts only for a document named `*.purpleplaylist` of at most 4 MiB.
  - It does not touch `Save`, so "Save As..." still saves the file.
- **A downloaded file.** One early return in `Data::ResolveDocument`, right after its null check (verified, `data/data_document_resolver.cpp:232`, check at `:239-241`): `if (Purple::OpenPlaylistShare(controller, document, item)) { return; }`.

Purple reads the file through the reader's in-memory path (section 8.2), never into Downloads.

**Strings** go in `Telegram/Resources/langs/purple.strings` (tdesktop `AGENTS.md`). Revision 3's "literals" is corrected.

### 6.2 Android

The refactor gave ChatActivity, DialogsActivity and the player sheet Purple hook objects, so most entries now go inside existing Purple methods.

**Chat message menu** (`ui/ChatActivity.java`).
- No new upstream line for the item. `fillMessageMenu` already ends with `purple.addSaveOptions(message, items, options, icons)` (verified, `:46782`). `PurpleChatHooks.addSaveOptions` inserts **Add to playlist** right after Save when Save is present, and otherwise before Delete. For a share file it inserts **Open as playlist**.
- One delegating case per option in `processSelectedOption` (verified, `:33628`), beside the existing `case PurpleChatHooks.SAVE_TO_GALLERY:` (`:33695`). The constants follow `PurpleChatHooks`' `0x7075xxxx` range (verified, `ui/PurpleChatHooks.java:54-65`). Revision 3's 1001 and 1002 are replaced.

**Chat multi-select (P5 c).** Three one-line hooks, unless `PurpleChatHooks` already wraps them:
- one call after the `save_to` item is created in `createActionMode` (verified, `:10392`), which adds a Purple item;
- one call next to `saveItem.setVisibility(...)` in the selection update (verified, `:19493`), which shows the item when the selection holds at least one qualifying song;
- one delegating branch in the action bar's `onItemClick`, beside `id == save_to` (verified, `:3855`). The existing `purple.onMenuItem(id)` dispatch (verified, `:3948`) may already route it, which would save this line (checked at implementation).

**Shared-media multi-select** (`ui/Components/SharedMediaLayout.java`).
- One call after `deleteItem` is added (verified, `:2265-2271`) adds a Purple item that owns its click listener, as upstream's items there do.
- The Purple item shows itself only in the Music tab, and only when the selection holds a qualifying song.
- If showing it needs a selection callback, one more call goes in `updateForwardItem()` (`:3904`).

**Opening a share file.**
- No new upstream line. The `TYPE_FILE` tap already calls `purple.importSettings(message)` first (verified, `ui/ChatActivity.java:41944-41945`). `PurpleChatHooks.importSettings` gains the share-file case.
- A tap on a file that is not downloaded yet goes to the cell's download button first (verified, `ui/Cells/ChatMessageCell.java:17872`). So on Android the first tap downloads the file and the next tap opens it, as with any file. **Open as playlist** in the message menu works at once.

**Player sheet** (`ui/Components/AudioPlayerAlert.java`). No new upstream line. `PurpleMediaSave.addAudioOption(o, messageObject)` already runs at the top of `showMenuOptions` (verified, `:2876`) and `showOptions` (verified, `:2952`). It gains **Add to playlist...** for that song and **Playlists**. That covers the case revision 3 needed a hook for, a protected song or the Profile music list, where upstream hides Save To (verified, `:2877-2882`). `QualifySong` still decides.
- **From the sheet, the picker opens with `sheet.show()`**, never through `showDialog`, which dismisses the dialog already on screen.
- Revision 3 also put **To Playlist** inside Save To (P8). Save To's submenu is built by `buildSaveOptions` and has no Purple call. Adding one costs an upstream line for an item that `addAudioOption` already offers one level up. This revision drops it; the owner can restore it in review.

**Picker** (`ui/PurplePlaylistPicker.java`).
- A sheet listing the account's playlists, most recently used first, with check marks. It behaves as in section 4.3, with **New playlist**.
- For an album song, a "Whole album (N songs)" switch sits at the top.
- For a multi-select, a header reads "N songs".
- The confirmation bulletin offers Undo and **Open**.

**Top-level entry (P8 b, choice P25).** Android has no side drawer; it uses bottom tabs (verified, `ui/LaunchActivity.java:592`).
- The row goes in the chat list's main menu, `DialogsActivity.showItemOptions()` (verified, `ui/DialogsActivity.java:13718`), right after the Saved Messages row (`:13820`).
- That menu already carries the Purple preset row through `PurpleDialogsHooks` (verified, `:13814`), so the playlist row can be added inside that Purple hook, with no new upstream line.
- The row is shown while `[playlists] menu_item_p` is true.

**Screens.**
- A **Playlists** row in `ui/PurpleSettingsActivity.java` opens `ui/PurplePlaylistsActivity.java`. It shows the list (recently used first), New, Recently deleted (with dates), the sync line and the note from section 6.1.
- That screen opens `ui/PurplePlaylistActivity.java`, which has:
  - Play, Shuffle and Download all;
  - rows, where a tap plays;
  - long-press for Remove, Show in chat and Use this copy;
  - drag handles using `ItemTouchHelper`;
  - Share, Rename and Delete in the top menu.

**Strings** go in the Purple block of `res/values/strings.xml`.

### 6.3 Naming next to upstream's Profile music

Unchanged, and P16 (a) stands.
- Both upstream clients already call Profile music a playlist: desktop's "Your playlist" (`lng_media_saved_music_your`) and Android's "Your Playlist" (`ProfilePlaylistTitleMine`, verified in use at `ui/Components/AudioPlayerAlert.java:1348`).
- Their Save To menus label it "Profile".
- Purple's feature is called **Playlists**. The wording is "Add to playlist", and the Playlists screen carries the note.

## 7. Library format and merge rules

This is the part whose meaning must never change. It lives entirely in purple-core and is pinned by property tests. Sections 7.2 to 7.10 are unchanged from revision 3.

### 7.1 Files

Each account has these files on each device, inside that account's directory as core's `ResolveAccountFiles` names it (verified, core `purple/purple_account_files.cpp:50-75`): the root of Purple's config directory for the startup owner, and `accounts/<env>-<user id>/` for every other account. Playlists add one field to `AccountFiles`, `playlists`, set to `<prefix>playlists`, beside the existing `sync` directory. Revision 3 placed the files in `tdata/purple-playlists-<env>-<user id>.json`, following `purple_last_seen.cpp:64`. The account directory is better now that it exists: both clients resolve it the same way, it is keyed by account, and the layer's binding (`sync/chat.json`) already lives there.

- **Library**: `<playlists>/library.json`. The contents are the canonical payload bytes, uncompressed so they stay inspectable. The file is written atomically (`QSaveFile` or `AtomicFile`) and read back. It is the only source of what the next publish carries, so a crash can never leave an edit that is never published (section 8.4).
- **`.bak`.** Before any merge or import that changes the library, the previous file is copied to `library.json.bak`, one level deep.
- **Local sync file**, `<playlists>/local.json`. It holds:
  - the switch `sync` (default true, P21), per account per device (section 8.8);
  - the sync status: the last step, its result, and `unsynced_since`, the time of the first local change not yet in a present record;
  - the sync chat token the memo belongs to;
  - the memo;
  - `old_chats`: chats this device left, with the memo entries of its own records still to delete there (section 8.12);
  - `due_seen`: for each playlist due for the automatic purge, when a complete step first saw it due (section 7.9);
  - `hidden`: the pre-merge content of playlists that a merge deleted or purged, kept until the person dismisses the notice or uses Keep a copy (section 7.8);
  - recent use (P19).

  Core parses this file and keeps unknown keys when it writes. It is never posted. Everything in it except `sync` and `hidden` can be derived again. Losing it costs one extra download and, at worst, one extra publish. Revision 3's list of chats confirmed for other readers is gone.
- **Record cache**, `<playlists>/records/`: the compressed bytes of each valid record in the memo, named by message id.
- **Share job and share history**, `<playlists>/share-job.json` and `<playlists>/shared.json` (section 10). Share history holds, for each destination chat, the document ids sent there and the copy map. It also holds each playlist's exchanged set.
- **Kept files**: on desktop the directory `<playlists>/files/`, and on Android the list `<playlists>/kept.json` (section 11).
- **Writers.** Each account has one serialized writer (desktop's main thread, or a single Android background queue), so a merge, an import and a local edit never interleave.
- **Logout** deletes the `playlists` directory when the session ends (section 8.10). It leaves the rest of the account directory alone, as settings sync does: "logout alone changes no identity" (`docs/purple/sync.md`, "Account-specific modern sync and recovery").

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
  - peer tokens parse with core `ParsePeerToken` (section 3.1);
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
  - **records:** at most **10 MiB compressed**, checked against the document size in the listing before downloading, and at most **32 MiB decompressed**. A record over either cap is Oversized and is never kept;
  - **share files:** at most 4 MiB compressed and 16 MiB decompressed;
  - **local edits:** refused past **24 MiB** (section 4.4);
  - **the local file:** no cap. A library that merges past 32 MiB still loads after a restart. It simply cannot be published until it shrinks (section 8.7).
- **Why these numbers.**
  - **10 MiB compressed** is desktop's limit for a download held in memory. `FileLoader` asserts, in release builds too, that a load with no target file is at most `kMaxFileInMemory` (verified, `storage/file_download.cpp:126`, and `storage/file_download.h:30`, where it is 10 MiB). The shipped reader now builds its own `mtpFileLoader` with an empty file name (verified, `purple/purple_sync_candidate_reader.cpp:255-270`), so the same assert applies. It is also the desktop Purple upload's own maximum (verified, `kMaximumUploadBytes`, `purple/purple_sync_config_upload.cpp:36`).
  - **32 MiB decompressed** gzips to about 9 MiB at the measured ratios, which fits.
  - **24 MiB for local edits** leaves room for 8 MiB of other devices' distinct data before a merged library outgrows what a snapshot can carry.
  - Realistic libraries are around 1 MiB (7,200 songs), so these caps are guards, not targets. H19 measures time and memory at both sizes.
  - The settings reader's cap is 4 MiB (`kSyncRecordMaximumBytes`, verified, core `purple/purple_sync_inventory.h:27`). The playlist mode of the reader passes its own cap.
- **Where gzip lives.** In core, so both clients share one guard and one set of tests. The new file is `purple/purple_playlists_codec.{h,cpp}`. Core gains a zlib dependency:
  - desktop already links `desktop-app::external_zlib` (verified, `Telegram/CMakeLists.txt:100`);
  - Android's `purplecore` links only `qt6core` and `log` (verified, `TMessagesProj/jni/purple_jni/purple.cmake:72-74`), so its list gains the NDK's `z`, which `tmessages` already links (verified, `TMessagesProj/jni/CMakeLists.txt:320`);
  - core's `tests/run.sh` gains `-lz`.

### 7.5 A local file this build cannot read

- **Missing:** the library is empty (first use).
- **NewerFormat:** the Playlists screen shows only "These playlists were saved by a newer Purple. Update Purple to open them." Edits, merges, imports and sync are disabled, and the file is never written.
- **Invalid** (corruption, or a hand edit): the screen shows "Your playlists file can't be read", with **Start over**.
  - Start over renames the file to `library.json.unreadable-<yyyymmdd-hhmmss>`, starts from an empty library, and clears the memo and the record cache.
  - The next step then restores whatever the sync chat holds.
  - Nothing else ever writes over an unreadable file.

### 7.6 Stamps

- **How a stamp is chosen.** An operation on playlist P takes s = max(now, the largest stamp or `since` anywhere in P, plus 1). Create uses now.
- **Where "now" comes from.** It is server-adjusted milliseconds where available.
  - Desktop combines `base::unixtime::now()` with `QDateTime::currentMSecsSinceEpoch() % 1000`, as the account setup already does (verified, `purple/purple_sync_account_setup.cpp:114`).
  - Android uses `ConnectionsManager.getCurrentTimeMillis()` (verified, `tgnet/ConnectionsManager.java:304`), a native call. Whether it includes the server time difference is H7.

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
- **Where it runs.** Only inside a playlist step, on the **merged** library, after that step's merge (section 8.5, step 5). It never runs before the merge, and never directly from the Playlists screen.
  - Running before the merge let a device that had been away for weeks purge a playlist other devices had since restored.
- **Conditions, with sync on.** All four must hold:
  1. **Discovery was complete in this step:**
     - the listing paged to its end;
     - every memo entry the listing missed was confirmed present or gone by id;
     - every new candidate was read, or classified as invalid, newer-format or oversized, with no transient failure and no Unknown ownership;
     - no memo entry was confirmed gone in a step that found no new valid record. A record that vanished without a new one appearing means another device pruned it after publishing a covering record this step cannot see yet.
  2. **No present record is newer-format.** A newer Purple may have restored the playlist in a record this build cannot read.
  3. **A request in this step succeeded** (the listing). MTProto servers ignore messages whose `msg_id` is more than 300 seconds in the past or 30 seconds in the future (docs, core.telegram.org/mtproto/description). Both clients correct their clock when the server reports a bad `msg_id`: desktop through `correctUnixtimeWithBadLocal` (verified, calls at `mtproto/session_private.cpp:1636`, `:1646`), which forces the update (verified, `:2175-2178`). So after a successful request the clock is within about 5 minutes of the server's.
  4. **The playlist was already due at an earlier complete step at least 15 minutes before** (`due_seen`). An entry is cleared when a complete step finds the playlist no longer due. In Saved Messages this absorbs search lag (H10). In the magic group the history read has no index lag, but the condition stays: it also covers a record another device posted and indexed after this step's listing, and it costs nothing.
- **Conditions, with sync off.** No other device can restore, so conditions 1, 2 and 4 do not apply. Condition 3 is met by one `help.getNearestDc` request, which Purple sends only when some playlist is due by the local clock, at the same times a step would run.
- **Who performs it.** Any device may. The purge is the ordinary Delete forever. It takes no stamp, and a union of purged sets is idempotent, so two devices purging the same playlist agree.
- **What it publishes.** The purge changes the merged library, so the same step publishes a snapshot and prunes older records (section 8.4). The purged playlist leaves the cloud at that step.
- **Remaining risks, accepted:**
  - A restore made within about 15 minutes of another device's purge is concurrent with it. The purge wins, and the restoring device shows Keep a copy.
  - s is at least the playlist's last edit plus one (section 7.6). If the deleting device's clock was more than 30 days behind, *and* the playlist had not been edited for 30 days, other devices purge it at once. Server-adjusted stamps make this rare. Restore before the date prevents the purge.

### 7.10 Future formats

- **A format 1 build** never merges or prunes a record, and never imports a share file, whose `format` is not 1. It keeps publishing its own format 1 records, and shows "A newer Purple changed your playlists. Update to see those changes." While a newer-format record is present, it also skips the automatic purge (section 7.9).
- **Pruning in v1** happens only between two format 1 records.
- **A constraint on the format 2 design:** it must define an upgrade from format 1. Format 2 code may prune a format 1 record only when a format 2 record read in the same step contains that record's upgrade.

## 8. Sync

### 8.1 Why playlist records stand apart from settings records

Settings sync is a different machine. A settings file cannot merge, so settings sync needs spaces, installs, sequences, a ledger, staging, reconciliation and clone checks (core `README.md`, `PlanSyncPublish` and the directory resolver; `docs/purple/sync.md`). A library merges by join, so it needs none of them: any device may publish any snapshot at any time, and the cover rule removes the rest.

Core does reserve a **library stream** inside the settings envelope (`SyncLocalStream::Library` and `SyncLocalState::library`, verified, core `purple/purple_sync_local_state.h:24-27`, `:72` on `sync-magic`), capped at 4 MiB and gated by `libraryPayloadValidated`, which nothing sets (core `README.md`; `docs/purple/sync.md`: "Library/playlist publishing remains disabled"). That was the account sync plan's earlier idea for playlists. This design does not use it, for four reasons:
- the stream publishes only after the account has joined or published settings sync, and the ledger requires a bound account (`CheckSyncAccountBinding` must return Bound). Playlists sync by default (P21) even when settings sync was never set up;
- its records are per install and versioned, with sequences and retirement of older ones. Playlist snapshots from any device can replace each other, which the cover rule proves safe without sequences;
- its cap is 4 MiB uncompressed JSON. Playlist records are gzip with a 10 MiB cap;
- a pending library stage refuses a chat move (core `docs/sync-chat-magic.md`, "Moving the state"), which would couple settings moves to playlist publishes.

This is choice P31. If the owner prefers one machine, playlists can ride the library stream with an edit-in-place own record per install, at the cost of the first reason.

Playlist records therefore use a name and a caption that the shipped settings rule rejects.
- Verified: `IsSyncHistoryCandidate` (core `purple/purple_sync_inventory.cpp:64-78`, unchanged since revision 3, and the same on `sync-magic`) accepts a non-forwarded document whose caption contains `#purplesync`, or whose name is exactly `Purple settings sync.json` or `Purple playlists sync.json` (`:52-62`).
- `Purple playlists.json.gz`, with the caption `Purple playlists sync data #purpleplaylists`, matches neither. The caption's only `#` begins `#purpleplaylists`, which does not contain `#purplesync`.

`Purple playlists sync.json` stays reserved in core and is never posted. It is what the settings inventory classifies as `UnsupportedLibrary` (verified, `:114`). Nothing in shipped settings logic is edited.

### 8.2 The record, the sync chat, and sending

- **The record.** A document in the account's sync chat, sent directly rather than forwarded, with the `silent` flag.
  - File name `Purple playlists.json.gz`, MIME type `application/gzip`.
  - Caption `Purple playlists sync data #purpleplaylists`.
  - Bytes: gzip of the canonical library (section 7.4).
- **How records are told apart in the shared chat.** The magic group holds settings records, playlist records and the group's own service messages. Each family has its own predicate, and the predicates are disjoint (C13):
  - **settings records**: `IsSyncHistoryCandidate` (above), then the envelope parser;
  - **playlist records**: `IsPlaylistRecordCandidate(meta, ownership)`, which requires a regular, non-forwarded message with a document named exactly `Purple playlists.json.gz`, **and** an ownership verdict of `Own`;
  - **share files** (`*.purpleplaylist`) never go to the sync chat; their detector exists only for chat taps.

  The file name is the record's identity. The caption tag exists for the Saved Messages search (section 8.3). A forwarded copy, a bot's message, or a document with the right name and the wrong sender is not a record.
- **Ownership.** The verdict comes from core's shared `GetSyncChatMessageOwnership(SyncChatMessageFacts)` (core `docs/sync-chat-core.md`, "Marks and ownership"; kept on `sync-magic-delete`), filled by the same client mapper as settings records (desktop `purple/purple_sync_chat_facts.{h,cpp}`, Android `messenger/purple/PurpleSyncChatFacts.java`). It refuses forwarded, via-bot, scheduled, TTL, non-regular and send-as messages. In the magic group it also needs `out` and a sender equal to the account. In Saved Messages it needs neither. **Unknown** is not Other: a record with unknown ownership is not memoized and counts as incomplete discovery, so it is never pruned and never blocks the step forever (section 8.5, step 3). This replaces revision 3's `out` check (H11) and its amendment 2.
- **What playlists need from the sync layer**, on each client:
  - `Lookup(account)`: run the layer's lookup and return **Use** with the bound chat and its kind (magic group or Saved Messages, or another manual chat), **Wait** with a reason and a time, or **Problem** with the layer's `SyncChatProblem`. Revision 3's `EnsureResolved` and `CheckBeforePost` both map to this one call: the layer already verifies a bound chat with fresh full info at most once a day before a post (Android `docs/sync-chat.md` on `sync-magic`).
  - **Chat-level and settings-level problems kept apart.** A problem about the chat itself (ChatLeft, NotMegagroup, PublicUsername, NotCreator, ExtraMember, ExtraAdmin, CannotPost, Restricted, MagicIneligible) pauses playlists too. A problem about settings only, such as Android's `space_mismatch`, does not.
  - **A change notice** when the bound chat changes, with the old and the new chat (section 8.12).
  - **Typed I/O routes** for the bound chat: history pages, exact reads by id, sends and deletes. Desktop has them in `purple/purple_sync_chat_io.{h,cpp}` (`docs/purple/sync_chat_io.md`) and Android in `PurpleSyncChatIO` and `PurpleSyncTelegramClient`.
  - **The per-method waits** the binding stores (`RecordSyncChatWait`, `SyncChatWaitUntil`), so a flood wait on `messages.getHistory` or `messages.sendMedia` holds for playlists as well as settings.
- **How Purple sends** records, share files and protected copies. Revision 3 asked for a Purple-owned upload and found none. Both clients now have one, built for settings records:
  - desktop `Purple::SyncConfigUpload` (verified, `purple/purple_sync_config_upload.{h,cpp}`): `upload.saveFilePart` in 32 KiB parts (`:37`, request at `:178`), at most 10 MiB (`:36`), then `messages.sendMedia` with `silent` through `DesktopSyncChatSendMediaRequest` (verified, `purple/purple_sync_chat_write.cpp:15-28`), or `messages.editMessage` for an edit in place;
  - Android `PurpleSyncChatUpload.request(...)` (verified, `messenger/purple/PurpleSyncChatUpload.java` on master and `sync-magic`): `TL_inputMediaUploadedDocument` with `force_file`, then `messages.sendMedia` with `silent`, or an edit; the parts are sent by `PurpleSyncTelegramClient` (verified, `:271` on master).

  Both hard-code the settings file name, MIME type and caption (desktop: `MTP_string("#purplesync")`, `purple_sync_chat_write.cpp:23-24`; Android: `PurpleSyncPost.RECORD_FILE_NAME`, `MIME` and `CAPTION`). Playlists generalize them: a `SyncChatUpload` that takes the file name, MIME type, caption, attributes and the `silent` flag, used by settings with today's values. Big-file parts (`upload.saveBigFilePart`, for files over 10 MB, docs api/files) are added for protected song copies only (section 10.5). The returned Updates go to the client's own update handling, as the settings path already does: desktop `ApiWrap::applyUpdates` (verified, `apiwrap.h:161`), Android `MessagesController.processUpdates` (verified, `messenger/MessagesController.java:17903`). The new message's id and document id come from the result (H27).

  **Why not the receipt plumbing.** It is still in upstream files: desktop `ApiWrap::sendFiles` with a receipt parameter (verified, `apiwrap.h:377-382`, implementation at `apiwrap.cpp:4346-4373`), used by the legacy settings post (verified, `purple/purple_sync.cpp:769`); Android `prepareSendingPurpleDocument(..., PurpleSendReceipt)` (verified, `messenger/SendMessagesHelper.java:9949`), which cannot send silently. Playlists never use it.
- **Readers.** The settings readers download a record's document by id. Desktop's builds its own `mtpFileLoader` from the server (verified, `purple/purple_sync_candidate_reader.cpp:255-270`), so revision 3's worry about reading a Downloads copy (the r2 finding G5) is settled. Each reader gains a playlist mode that takes the bound chat, tests `IsPlaylistRecordCandidate`, caps the download at 10 MiB, and returns raw bytes. Both readers are Purple files.
- **Auto-download.** Desktop already skips automatic loading for sync record names in one Purple guard (verified, `data/data_document_media.cpp:314`, calling `Purple::IsSyncRecordFileName`). Android excludes the two reserved names (verified, `messenger/purple/PurpleSyncAutoDownload.java`, `isRecordName`). Both gain `Purple playlists.json.gz` and `*.purpleplaylist` inside the Purple functions, with no upstream line. Official clients may still auto-download records when the group is opened (H8); the group is archived, muted and owned by one person, so this only costs that person's bandwidth.

### 8.3 Discovery

**In the magic group (the default): read the history.**
- The step pages `messages.getHistory` (schema verified, `api.tl:2595`) through the bound group, 100 messages per page, from the newest, until a page comes back short of 100 or empty, or until it has seen `count` messages. The settings inventory reads the same chat the same way (Android `PurpleSyncScanner.PAGE_SIZE = 100`, verified on `sync-magic`; desktop `purple/purple_sync_history_scanner.cpp`).
- The group holds only Purple's records and a few service messages, and both families prune their own old records, so one page is the normal cost. That is no more than revision 3's one search.
- A history page has no search-index lag. A record is listed as soon as it exists. So H10 no longer applies here, and a missed record can only come from a race with a concurrent post.
- The step keeps the messages that pass `IsPlaylistRecordCandidate`. Settings records and service messages are skipped.
- **Shared pages, later.** When the settings check and the playlist step run within the same minute, one listing could serve both. Not in v1: it couples two state machines for a saving of one request.

**In Saved Messages (manual binding): search, as revision 3.**
- `messages.search` (schema verified, `api.tl:2596`) on Saved Messages with `q = "#purpleplaylists"`, `filter = inputMessagesFilterDocument` (`api.tl:309`) and `limit = 100`. It pages by `offset_id` until a page comes back empty, or until it has seen `count` messages. It does not stop at the first short page.
- Precedent: the shipped Saved Messages settings search (verified, `purple/purple_sync.cpp:707`).
- **If the tag search fails (H10).** Fall back to the file name, then to an empty query with the document filter, paging over every document.

**In another chat bound by hand** (no other member or admin, per the layer): read the history, as in the magic group, but stop at 20 pages and fall back to the search above. A chat with long history is the person's choice, and searching keeps the cost bounded.

**Why a miss does no harm.**
- A record a listing missed is merged at a later step.
- A missed record is never pruned, because pruning needs a covering record that was actually read.
- A memo entry the listing missed is confirmed by id before it is dropped (section 8.5, step 2).
- The automatic purge waits out lag (section 7.9).

There is no global search and no startup marker. The layer finds the chat from the local chat list, with `messages.getAllChats` only as the absence proof (core `docs/sync-chat-magic.md`, "Lookup").

### 8.4 When a step publishes, and why snapshots only

Unchanged from revision 3.

**The rule.** A step publishes a snapshot of the merged library whenever no present valid record already equals it. That single rule covers these cases:
- **An edit here.** The local library changed, so no record equals it.
- **A first publish, a changed sync chat, or a Start over followed by a merge that gained nothing.** No present record is this device's.
- **Two devices that published concurrently.** The next step on either one merges both, finds that neither equals the join, and publishes the join. That covers both, and they are pruned in the same step.
- **Records left by a failed prune, or one record covered by another.** No publish is needed, because some present record equals `merged`. The cover check prunes the others when the present set changes.
- **A purge.** The purge changes `merged`, so the step publishes and the purged playlist leaves the cloud.
- **A removal.** Its chat leaves the cloud at the next publish, within about 10 minutes under automatic sync.

**Crash safety.** What gets published is decided from the library file alone, which is written atomically. A crash at any point leaves at worst one extra publish or one extra download, never an edit that no record will carry.

**Why snapshots only in v1** (choice P30):
- The owner asked about performance and whether every sync merges (P15). The memo answers that without deltas: the steady state costs one listing, and each device downloads each snapshot once.
- A snapshot of a realistic library is small: about 25 KB gzipped for 500 songs, 98 KB for 2,000 and 350 KB for 7,200 (measured). Automatic sync publishes at most once per burst of edits.
- Format 1 still accepts partial playlists, so a later version can add deltas without a format bump if H19 or real use shows that snapshot traffic matters.

**Post and prune, not edit in place** (choice P36). Settings sync now edits its own record in place (`messages.editMessage`, `docs/purple/sync.md`, "Automatic settings sync on Desktop"), and the generalized upload can do the same. Editing this device's own latest record instead of posting a new one would save a delete per publish and keep the message count lower. v1 still posts and prunes, because the cover proof (section 8.5.1) is written for immutable records and every device can prune any covered record. An edited record gets a new document id, so the memo already reads it afresh; edit in place is safe to add later, with C8 extended to edits.

### 8.5 The playlist step

It runs:
- automatically (section 8.6);
- from **Sync now** on the Playlists screen;
- as the final step at logout (section 8.10).

It never runs from the settings Check.

**Preconditions:**
- the library is readable (section 7.5);
- the account's `sync` switch is on, or the step is a manual Sync now or the final logout step;
- no step is already running for that account in this process.

**The steps:**

1. **Resolve the chat.** Call the layer's `Lookup(account)`. On **Wait** or **Problem**, stop with that status (section 8.7). On **Use**, take the bound chat and its kind. If the chat differs from the one the memo belongs to, run section 8.12 first.
2. **List** (section 8.3), then **compare with the memo.**
   - **New** candidates are those whose (message id, document id) pair is not in the memo. A record whose media was edited has a new document id, so it counts as new.
   - **Missed** candidates are memo entries the listing did not return. The step fetches them by id in one request: `channels.getMessages` in the magic group, `messages.getMessages` in Saved Messages.
     - Those still there, and still passing the predicate, count as present.
     - Those gone lose their memo entry and cache file.
     - If that request fails, the step still continues, but discovery counts as incomplete.
   - Memo entries with a non-valid status whose classifier version differs from this build's are treated as new, so an upgrade re-reads them.
3. **Read the new ones.** Download each, up to 10 MiB, then decode it (up to 32 MiB) and validate it.
   - A valid record gets a memo entry with the hash of its canonical bytes, and its compressed bytes are cached.
   - An invalid, newer-format or oversized record gets a memo entry with that status and the classifier version.
   - A download that failed, a candidate that vanished or was inaccessible, or a candidate whose ownership is Unknown, gets **no** memo entry and is tried again next step.
4. **Merge.** `merged` = the join of the local library and every new valid record. Records already in the memo were merged before, and the local library only grows, so they need no second join.
5. **Plan.** Call core `PlanPlaylistStep(local, merged, present records with status, hashes and cached bytes, discovery completeness, the request-succeeded flag, due_seen, now, sizes)`. It returns:
   - `purged`: the playlists the automatic purge removes now (section 7.9), applied to `merged`, and the new `due_seen`;
   - `writeLocal` and `hidden` (with pre-merge content), and a summary of added and removed songs;
   - `publish`: true when no present valid record has the canonical hash of `merged`, `merged` is not an empty library with no records present, and `merged` fits the record caps. Otherwise false, with a reason;
   - `prune`: the message ids of present valid records that another present valid record covers. The contents come from the cache. The checks run only when the present set changed, or when this step publishes;
   - counts of invalid, newer-format, oversized and unreadable records.
6. **Write locally.** If `writeLocal` is set, write the `.bak` and then the new file, save `hidden` into the local sync file, refresh the UI, and show one notice per hidden playlist.
7. **Publish.** Call `Lookup(account)` again if the first call is more than a minute old, so a chat that became ineligible is not posted to. Encode `merged` and send it through the generalized upload (section 8.2) with `silent`. On success:
   - add the new record to the memo, with its hash and cached bytes, from the bytes in hand;
   - if the local library has not changed since this step began, clear `unsynced_since`.

   The just-published record then counts as read in this step. It contains every present valid record, because `merged` does, so core verifies containment for each one from the cache and prunes them now.
8. **Prune.** Delete each id in `prune`: `channels.deleteMessages` in the magic group (schema verified, `api.tl:2901`), `messages.deleteMessages` with `revoke` in Saved Messages (`api.tl:2599`). The account is the group's creator and sole admin, so it may delete (H3). Failures are ignored and retried at the next step. Pruned ids leave the memo and the cache.
9. **Show status** (section 8.9).

**The steady state costs one request**: one history page in the magic group, or one search in Saved Messages. The page lists the records the memo holds, and the local library's hash equals one of them. So steps 3 to 8 do nothing: no download, no decompression, no parse, no join and no write. The layer's own lookup costs no request while the bound chat is in memory and its full info is under a day old.

**Is there a merge on every sync?** That was the owner's question under P15. No:
- a step that finds nothing new costs one listing;
- a step that finds another device's new snapshot downloads it once, about 25 KB gzipped for 500 songs and about 350 KB for 7,200 (measured), and joins only that;
- the local file is rewritten only when the merge changed it.

#### 8.5.1 The cover rule, and why it cannot lose data

Unchanged from revision 3. A device deletes record R only when it has read a record O that covers R, and both are valid format 1 records. "Read" means one of three things:
- downloaded in this step;
- taken from the cache under the same message id and document id that the listing, or the fetch by id, returned in this step;
- published by this device in this step.

**Why no data is lost.**
- Records are libraries, full or partial, and absent registers are the bottom element. Containment is therefore the order of one join-semilattice: reflexive (join(R, R) = R), antisymmetric in canonical form, and transitive, since join is associative.
- Covering adds the message-id tie-break, so it is a strict order, and no two records cover each other.
- Take any data in a deleted record R. Some record O covered R. If O was deleted too, some record P covered O, and then P also covers R.
- Posted records are finite, so this chain ends at a record nothing covers. No device can delete it, because covering depends only on content and message id, which every device sees alike. That record holds the data.
- Every device's local library only grows by join, import and local operations, so it still holds everything that device ever published.
- Pruning never changes the join of the records present, because each pruned record lies below a remaining one. That is also why the memo stays correct across prunes.

**The memo does not weaken the rule.**
- A cached record is used only under the same message id and document id that this step's listing or fetch returned.
- Records are never edited in v1, and an edited record gets a new document id and is read afresh.

**Two assumptions**, both stated in the docs: nobody edits a record's media by hand, and nobody deletes records by hand. A hand deletion loses only data that lived solely on a lost device. The next publish by any live device puts everything it holds back. The magic group being hidden and owned by one person makes both rarer than they were in Saved Messages.

### 8.6 When steps run (P4)

The automatic behaviour runs while `[playlists] auto_sync_p` is true, which is the default:

- **After edits.** Every local operation or import restarts a **60 second quiet timer** for that account. The step runs when the timer fires, or **10 minutes** after `unsynced_since`, whichever comes first. If a step is already running, one more is queued.
- **Checks.** At app start, when the app returns to the foreground or is unlocked, and when the Playlists screen opens, a step runs if the last step for that account is at least **15 minutes** old. These are the same triggers and the same 15-minute gate as automatic settings sync (`docs/purple/sync.md`, "Automatic settings sync on Desktop"), so both families check together and the layer's lookup is shared. There is no periodic polling timer.
- **Failures.** An automatic step that failed, for example offline, retries after 1, 2, 5 and 10 minutes, and then every 10 minutes while the library differs from every present record. A flood wait stored in the binding moves the retry to its end.
- **Manual.** **Sync now** always runs a step, whatever the options say, but still honours stored waits.
- **Limits, stated.** Timers run while the process lives. On desktop, quitting before the timer fires leaves edits waiting until the next start. On Android, a killed process does the same. The library is on disk, so nothing is lost, only delayed.

With `auto_sync_p` false, only **Sync now** and the final logout step run.

**The scheduler.** A per-process `PlaylistScheduler` iterates every signed-in account, so accounts that are not on screen still get the app-start check (revision 3's amendment 4).
- Desktop: started from `Purple::StartAppServices()` (verified, `core/application.cpp:354`), which already exists. It follows `Core::App().domain().accounts()` (verified, `main/main_domain.h:77`) and each account's `sessionChanges()` (verified, `main/main_account.h:80`). No new upstream line.
- Android: started from `PurpleDialogsHooks#onFragmentCreate` (verified in use, `ui/DialogsActivity.java:2844`), which already exists, over `UserConfig.isValidAccount`.
- It also registers the session-end observers of section 8.10.

H29 checks that the scheduler can list, step and upload for an account that is not active on desktop, or not selected on Android, without that account's UI.

### 8.7 Failure cases

- **Invalid record:** ignored and never pruned. The status counts it: "1 playlist sync message can't be read". The person can delete it by hand (showing the group first, through the layer's "Show sync chat in chat list").
- **Newer-format record:** ignored and never pruned. The status asks for an update, and the automatic purge waits.
- **Oversized record** (over 10 MiB compressed, or expanding past 32 MiB): never downloaded or kept, and never pruned. The status counts it.
- **A candidate that vanished, failed, was inaccessible, or had Unknown ownership:** skipped this time, not added to the memo, and discovery counts as incomplete.
- **Post failed:** the status says so, and the next step tries again, because the library still differs from every record.
- **Delete failed:** retried at the next step.
- **Merged library too large to publish** (over 32 MiB, or over 10 MiB compressed): reading and merging continue, but this device publishes nothing. The status says "Playlists are too large to sync from this device. Delete forever deleted playlists, or remove songs." Local edits are refused above 24 MiB.
- **The layer says Wait** (no binding yet, creation in progress, a flood wait, a partial chat list): "Setting up sync…" or "Telegram asked Purple to wait until <time>". Nothing is posted; the next trigger after the wait tries again.
- **The layer reports a chat problem:** the layer's own wording, plus "Playlists are paused until this is fixed · Open sync settings". Nothing is posted anywhere else. When the person fixes it, the layer clears the problem and the next step runs.
- **Switch off:** nothing is listed, sent or deleted. Existing records stay in the sync chat until the person removes them.

### 8.8 Options

- **The sync switch** is `sync` in the local sync file, per account per device, default on (P21). Its wording is "Turns playlist sync off for <account> on this device." This keeps revision 3's amendment 3: a switch that writes `settings.toml` would reach every device through settings sync, while the first-run consent it pairs with is per device.
- **In settings.toml, under `[playlists]`**, read through the per-account layering (P10):
  - `auto_sync_p` (default true): the automatic behaviour of section 8.6 (P4);
  - `menu_item_p` (default true): the top-level Playlists row (P8);
  - `keep_downloaded_p` (default true): section 11 (P6).
- **In settings.toml, under `[playback]`**: `private_listening_p` (default true), P20 (section 5.1).

**How per-account layering works today.** P10 shipped as one settings file per account, layered over the global file key by key, not as sections inside one file. Core's `LayerSettings(globalText, globalPath, accountText, accountPath, options)` merges them (core `docs/settings-layering.md`). Account files live at `accounts/<env>-<user id>/settings.toml` (verified, core `purple/purple_account_files.cpp:61-63`), and desktop reads each account's effective settings through `DesktopAccountConfig` (`docs/purple/account_routing.md`). Each account's settings sync carries that account's own file. So:
- core's `purple/purple_settings.{h,cpp}` gains the `[playlists]` readers and `private_listening_p`, with defaults, and `docs/purple/config.md` documents them;
- the Playlists screen's Automatic sync toggle writes the active account's file when it already sets the key, and the global file otherwise, with the existing surgical splice. Its subtitle says which: "For all accounts on every device that syncs settings" or "For <account> on every device that syncs its settings";
- revision 3's worry that a section keyed by user id would leak one account's id into another account's sync chat is gone: no account's id is written into another account's file.

### 8.9 The sync line and the first-run bulletin

The Playlists screen on each client has one sync line. It shows one of:
- "Syncing…";
- "Synced 5 min ago · Sync now";
- "Changes waiting to sync · Sync now", while `unsynced_since` is set. The Add toast repeats it as a second line only when automatic sync is off;
- "Offline: changes will sync when you're connected";
- "Sync is off · Turn on";
- the problems in section 8.7, and "A newer Purple changed your playlists. Update to see those changes."

The screen also shows "Sync chat: Magic Purple Sync · Sync settings" (or "Saved Messages"), which opens the layer's Sync chat screen for that account.

**The first-run bulletin** appears where the account's first playlist is created: in the picker, as a bulletin, or at the top of the Playlists screen.
- With a bound chat: "Your playlists sync through your private Magic Purple Sync group, so your other devices get them. Only you are in it. Learn more · Turn off".
- With no binding yet: what P33 decides. By default the layer's lookup runs and may create the group (choice P33), and the bulletin says "Purple will create a private group named Magic Purple Sync for this. Purple hides it; other Telegram apps show it archived and muted. Learn more · Turn off".
- **Learn more** shows the disclosure (section 9).

Revision 3's confirmation for chats with other readers, its "waiting for your confirmation" status and its public-chat status are removed: the layer allows no such chat.

### 8.10 Logout (P18)

Unchanged in substance from revision 3. Lines are updated.

**The mechanism has two parts.**
- **A hold** before the logout starts, which runs a final step or shows a dialog.
- **A deletion** when the session actually ends, observed from Purple code:
  - desktop: an observer on `account->sessionChanges()` for each account (verified, `main/main_account.h:80`) sees `nullptr` while `!Core::Quitting()` (verified, `core/application.cpp:2171`). The observer remembers each account's user id while the session is live. The session is also destroyed on quit, now with `DestroyReason::Quitting` (verified, `main/main_account.cpp:58-64`, enum at `main/main_account.h:123-126`), which is why the quitting check is needed;
  - Android: an observer of `NotificationCenter.appDidLogout` for each account, which `performLogout` posts for every logout type (verified, `messenger/MessagesController.java:16207`). `performLogout` clears the user config before posting it (verified, `:16175`), so the observer uses the user id it recorded while the account was live.

  Both observers are registered by the scheduler (section 8.6), so no upstream hook is needed. H26 checks that they fire for user, forced and forgotten-passcode logouts, and never on quit.

  **A precedent, and an alternative.** Purple now hooks logout directly on desktop: `Account::logOut` and `Account::forcedLogOut` each call `Purple::StopEmojiExportAtLogout` and `Purple::RemoveArchiveAtLogout` (verified, `main/main_account.cpp:540-541`, `:557-558`). Adding playlists there would be two more upstream lines. A cleaner option is one tdesktop refactor commit that folds those calls into a single `Purple::OnAccountLogout(*this)` facade, which playlists then join. The observer stays the default because it needs no upstream line at all.

**Why deletion waits for the session to end.** On desktop, `logoutWithChecks` goes on to ask about running exports, uploads and downloads, and each of those prompts can cancel the logout (verified, `core/application.cpp:1041-1062`). A hook that deleted files before them could leave a signed-in account with its playlists gone.

**With sync on:**
- The hold runs a final step with the progress line "Syncing playlists before logging out". If the library already equals a present record in the memo, it skips the step, so an offline device can still log out without a prompt.
- On success, the hold calls upstream's `retry`. The second pass consumes a one-shot flag and returns false, so upstream's own checks then run. If one of them cancels, the flag is already gone and the files are untouched. The next logout attempt runs the (cheap) final step again.
- If the step fails with changes waiting: "Couldn't sync your playlists. Log out anyway and delete them from this device? Changes since <time> will be lost." with **Try again**, **Log out anyway** and **Cancel**.
- **When the layer cannot give a chat** (Wait or Problem): "Playlists of <account> aren't synced yet: <the layer's reason>. Log out and delete them from this device?" with **Open sync settings**, **Log out and delete** and **Cancel**. The hold waits at most 10 seconds for the lookup before showing it.

**With sync off:**
- The hold warns: "Playlists of <account> aren't synced. Logging out deletes them from this device."
- The buttons are **Sync once, then log out** (one step whatever the switch says, offered only when the layer has a chat), **Log out and delete** and **Cancel**.
- With no playlists, nothing is shown.

**A forced logout** (the session ended elsewhere) cannot sync. By default the session-end observer deletes the files, as upstream wipes the account's local data (verified: `Account::forcedLogOut` calls `loggedOut()`, which calls `local().reset()`, `main/main_account.cpp:555-569`). A remote session end is often a lost or stolen device, and with automatic sync the unsynced window is normally under 10 minutes. The docs state that edits from those last minutes can be lost. Choice P27 keeps the alternative.

**Hooks:**
- Desktop: one guarded early return at the top of `Core::Application::logoutWithChecks` (verified, `core/application.cpp:1041`): `if (account && account->sessionExists() && Purple::PlaylistsHoldLogout(account, retry)) { return; }`.
- Android: one guarded early return at the top of `MessagesController.performLogout(int type)` for `type == 1`, the user-initiated logout (verified, `messenger/MessagesController.java:16157`). Nothing after it can cancel the logout.

### 8.11 What other builds see

- **Purple builds without playlists** ignore playlist records. Verified: the settings rule (section 8.1). Their settings inventory reads the same magic group and skips the records, because `IsSyncHistoryCandidate` is false for them.
- **Older Purple builds that still use marks** (core `sync-magic-delete` notes that older clients still use marks) look for `#purplechatsync` text messages. Playlist records are documents without that tag, so those builds ignore them too. C13 keeps the mark text among its negative inputs.
- **Official clients** show the group archived and muted (when those cosmetic steps succeeded), with gzip documents in it.

### 8.12 When the bound chat changes

The layer can bind another chat: a device that created its own magic group rebinds to the oldest one so all devices converge (`PickMagicChat`, lowest channel id; core `docs/sync-chat-magic.md`, "Lookup"); the person picks Saved Messages or another chat by hand; or the bound group became ineligible and the person chose a fix. Revision 3's procedure read the old chat, published into the new one and then deleted the old records, and its amendments routed that through the layer's carry journal. The carry journal is gone (core `sync-magic-delete`, "Remove sync-chat marks, resolution and carry"). The new procedure needs no reading of the old chat:

1. **Publish into the new chat.** The memo belongs to the old chat, so it is moved into `old_chats` and the step starts with an empty memo in the new chat. The step lists the new chat, merges whatever it finds, and publishes a snapshot (section 8.4). Nothing is lost by not reading the old chat: every device's local library already holds everything it ever published, and every device that still uses the old chat will rebind and publish from its own library.
2. **Clean up the old chat, best effort.** After a successful publish in the new chat, for each entry in `old_chats` whose cached record the new snapshot contains (checked in core from the cache), delete that message with the old chat's method. Records this device never cached (other devices' records) are left alone, because this device cannot prove they are covered. Each device cleans up its own memo's records when it moves.
3. **Give up quietly.** If the old chat is unreachable (left, deleted, no rights) or a delete fails three times, drop the entry. The old chat was a private chat of this account, so leftover records reveal nothing to anyone else.

The layer moves the settings state on its own (`MoveSyncStateToChat`). Playlists do not take part in it and do not block it.

## 9. Privacy: what leaves the device

**Disclosure**, behind the first-run bulletin's **Learn more** and on the Playlists screen:

> Playlists of <account> sync through <Magic Purple Sync, your private group that only you are in | Saved Messages>. Purple posts a file named "Purple playlists.json.gz" there. Telegram's servers and every device signed in to <account> can read it. It is not end-to-end encrypted.
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
- anything from `settings.toml`, `state.toml` or the binding;
- any device name, platform, app version or install id.

Acceptance X6 scans every posted record for these.

**Local only:** the library, its `.bak`, the local sync file, the record cache, the share job and share history, and kept files. They sit in the account directory as plain files, like settings and the binding. Kept audio on desktop sits outside the media cache, which is encrypted with the account's local key (verified, `Account::cacheKey`, `storage/storage_account.cpp:1884`). The deleted-message archive now shows that Purple can encrypt local data with the account key (core `docs/archive-storage.md`). Encrypting the playlist files the same way is choice P37; the default keeps them plain and inspectable.

**Logs.** Playlist code logs counts, statuses and error types only. It never logs titles, peer tokens, usernames, document ids, destinations or sender names. That matches the existing sync code, which logs only the error type (verified, `purple/purple_sync.cpp:740`). On Android, playlist lines go through `PurpleDiagLog`, whose repeat limit the media work added. X6 also scans the logs.

With the `sync` switch off, nothing playlist-related leaves the device, except shares the person sends on purpose.

## 10. Sharing

Unchanged from revision 3 except where noted: protected copies are no longer blocked on a user-control layer (section 10.5), the send path reuses the generalized upload, and lines are updated.

### 10.1 What it does

Share sends a playlist to a chat:
- **every playable song**, forwarded with `drop_author` and `drop_media_captions` (schema verified, `api.tl:2604`). Recipients see plain audio messages **from the sharer**, with no "Forwarded from" header and no caption. This works for recipients on any Telegram client;
- then **a share file** that a Purple recipient opens as a playlist, or merges into an earlier import of the same playlist after a preview.

Songs from protected chats cannot be forwarded. They are re-uploaded from this device after a confirmation (section 10.5).

### 10.2 Identity: the playlist id itself

**Decision:** the share file carries the playlist's own id, and the recipient's import keeps that id. There is no separate share id.

**Why.**
- **Concurrent imports converge by themselves.** A recipient who imports the same share on two devices before they sync gets two writes under one id, and the join merges them.
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
   - Desktop uses `Window::ShowChooseRecipientBox` (verified, `window/window_peer_menu.h:198`). Its `chosen` callback accepts only a thread where the account may send music and files (verified, `ChatRestriction::SendMusic` and `SendFiles`, `data/data_chat_participant_status.h:62`, `:64`).
   - Android uses `DialogsActivity` in select mode with `DIALOGS_TYPE_FORWARD`, with the same rights checks (verified, `ChatObject.canSendMusic`, `canSendDocument`, `messenger/ChatObject.java:2189`, `:2196`).
   - **Refused, with a one-line reason:**
     - a group with slow mode, unless the account is an admin. Docs: forwarding several messages there fails with `SLOWMODE_MULTI_MSGS_DISABLED`;
     - **a chat that charges for messages**. Desktop checks `starsPerMessageChecked() > 0` (verified, `data/data_peer.h:336`), and Android checks `MessagesController.getSendPaidMessagesStars(dialogId) > 0` (verified, `messenger/MessagesController.java:7257`). `messages.forwardMessages` takes `allow_paid_stars` (verified, `api.tl:2604`), and upstream handles `ALLOW_PAYMENT_REQUIRED` in its own share and send paths (verified, `boxes/share_box.cpp:1970`, `apiwrap.cpp:534`). Purple's requests skip that handling, so they would fail batch after batch;
     - the account's own sync chat. Shares are for people.
3. **Resolve** every visible song in display order, with fallbacks, and take a snapshot of the playlist at that moment. The file describes that snapshot.
4. **Classify** each song:
   - **forwardable;**
   - **protected:** on desktop `HistoryItem::forbidsForward()` (verified, `history/history_item.h:490`); on Android the message's `noforwards`, or `MessagesController.isPeerNoForwards` (verified, `:6723`). The copy policy that makes such songs copyable locally does not change forwarding: the server still refuses it (docs, and `docs/purple/user_control.md`: "These actions do not change forwarding permissions");
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
  - Android's own forward path cuts batches at 100, and at every change of source chat, and sets `drop_author` and `drop_media_captions` from the "hide sender" and "hide caption" options (verified for revision 3; `messenger/SendMessagesHelper.java` forward path).
  - Desktop's `ApiWrap::forwardMessages` (verified, `apiwrap.cpp:3902`) sends one request per run of items from one source, and sets `f_drop_author` and `f_drop_media_captions` the same way (verified, `:3985`, `:3988`). It does not cut at 100, and it does not report the new ids.
  - Docs: the method page names `drop_author` and `drop_media_captions`, but states no id limit. The 100 comes from the clients (H14).
- **Purple sends its own requests** from a Purple file on each client, through the same Purple request senders the sync code uses, with its own random ids. It passes the resulting updates to the client's update handling (section 8.2). This needs no upstream edit, and gives Purple control of batching, pacing, random ids and errors.
- **Order.** Batches go strictly one at a time, each after the previous response. Within a batch, ids go in playlist order. If the server orders them by id instead, non-Purple recipients see chat order within that batch (H14). The share file carries the true order either way.
- **Notifications.** Song batches are sent `silent`. The share file is sent normally, so the recipient gets one notification instead of hundreds.
- **Flood waits.**
  - On `FLOOD_WAIT_X`, and on `FLOOD_PREMIUM_WAIT_X` (docs, api/errors), the job waits X seconds and shows "Telegram asked Purple to wait 38 s". It then retries the same batch with the **same random ids**. The wait is stored per method, as the sync waits are, so a restart honours it.
  - A batch whose response was lost is retried the same way. The server's duplicate detection should then create no second copy (H17), and a duplicate-id error counts as sent.
- **Errors.**
  - `CHAT_FORWARDS_RESTRICTED` (docs: "You can't forward messages from a protected chat") on a batch the check thought forwardable means protection changed in the meantime. Those songs move to the protected path.
  - An error for one batch marks its songs as failed, and the job continues.
  - Losing the right to write to the destination, or a paid-message error, stops the job.
- **Persistence.** The job is saved in `share-job.json` after every batch: the destination, the snapshot, per-song state, per-batch random ids, and the copies made.
  - After an interruption, the next start shows "Sharing “Evening” to <chat> stopped at 120 of 420. [Resume] [Discard]".
  - **Cancel** stops after the current request and discards the job, and no file is sent.
  - Each account has at most one job at a time.

### 10.5 Songs from protected chats

**What changed.** Revision 3 shipped sharing with "skip these songs" until a user-control setting existed. The policy now exists, always on, on both clients (section 3.3). So the dialog below is shown from the first sharing release. What it still waits for is the big-file part of the generalized upload: most songs are over 10 MB.

**The dialog**, asked once per share action, with no "don't ask again" (revision 3's amendment 11):

> 7 songs are from protected chats. Their owners turned off forwarding and saving, so Telegram won't forward them. Purple can upload copies from this device instead: this downloads 18.4 MB for songs not yet on this device and uploads 41.2 MB. The copies are not protected: anyone in <chat> can save and forward them.
>
> [Upload copies] [Skip these songs] [Cancel]

- The sizes come from the documents' sizes. The download figure counts only songs without a complete local file.
- **Skip these songs** leaves them out of the forwards and out of the file.
- If the owner later makes P13 a switch and turns it off (choice P34), there is no dialog, and the confirmation says "7 songs from protected chats will be left out."

**Uploading a copy**, at the song's place in order:
- **Getting the bytes, without moving the document's recorded location:**
  - desktop reads, in order: the kept file (section 11); the document's own local file, if `location(true)` finds one readable (verified, `data/data_document.cpp:1440`); the in-memory bytes. It never calls `DocumentData::save` to a temporary path. That call sets the document's location to the path whenever the bytes are in memory (verified, `:1227-1243`), and deleting the temporary file afterwards would break a kept song's offline playback;
  - if no local copy exists, desktop downloads the song into the kept directory, as Download all does. If Keep downloaded is off, that file is deleted after the upload. A missing file only makes the next play stream, because `location(true)` re-checks (verified, `core/file_location.cpp:78`);
  - Android uses the downloaded media file, downloading it first if needed.
- **Sending** goes through the generalized upload (section 8.2) as an audio document with the song's audio attributes, with `silent` set, using big-file parts when it is over 10 MB. The new message's document id is recorded as the copy's id.
- The copy carries the file's own tags. Where the original's Telegram attributes differ from its tags, the copy shows the tags (H18).

**Why a copy is needed.** Docs, api/files: "Non-protected media (i.e. media from messages without the noforwards flag) may be resent to other chats, without reuploading it". A protected song therefore has no server-side shortcut.

**Two facts for the owner,** unchanged: Telegram's content-protection guidance asks clients to treat protected messages as if forwards, downloads, copying and screenshots must be disabled (docs, api/content-protection, via revision 3's terms research), and the terms research records that enforcement falls on the api_id owner. The server sees a copy as a fresh upload. The dialog makes the choice explicit to the person; it does not change those facts.

**Identity and the copy map.**
- In the share file, a copied song is keyed by the **copy's** document id, with the original's display fields, `since` and stamp. The sharer's own library still points at the original.
- The copy map lives in share history on the device that made the copies.
  - **When building a file**, that device substitutes copies in `songs`, in `order` and in tombstones. A later removal of the original therefore reaches recipients as a removal of the copy.
  - **When importing a file**, that device maps each copy id back to its original before merging. For a mapped song, only a tombstone is adopted. A non-`false` value never overwrites the local original, whose source is the real one.
- **A re-share from the same device** reuses an earlier copy already in that chat.
- **Known limit.** The copy map is not synced. A re-share from another device of the sharer uploads new copies, and an import on another device of the sharer adds the copy beside the original. The person removes the duplicate by hand.

### 10.6 The share file

- **Name:** `<title>.purpleplaylist`.
  - The title is sanitized: characters other than letters, digits, spaces and `-_().` become `_`, which includes `#`.
  - It is cut to 60 code points, and becomes "Playlist" if empty.
  - MIME type `application/gzip`.
- **Caption:** "Playlist file with 42 songs. Open it in Purple Telegram to save the songs above as a playlist."
  - It carries **no title and no `#`**. A title such as "#purplesync mix" in the caption would make the file a settings-sync candidate on every build: `IsSyncHistoryCandidate` accepts any non-forwarded document whose caption contains `#purplesync` (verified, core `purple/purple_sync_inventory.cpp:64-70`).
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
  - **a tombstone** (document id and stamp) for each removed song **whose document id is in this playlist's exchanged set** on this device, with copies substituted.
- **It never carries:** `peer`, `msg`, `un` or `alt`; the deleted flag or the purged set; other playlists; the sharer's user id; device data. So it cannot name the sharer's source chats or message ids. C22 is a property test that no such field can appear.
- **What it does reveal, and the disclosures say so:** when each song was added and last changed, to the millisecond, because merging depends on stamps; and the playlist id, which links shares of one playlist.
- **Why it holds no destination message ids either.** In private chats and basic groups, each side numbers messages separately (section 3.1). Recipients find the songs by document id instead, which forwarding keeps (section 10.7).
- **Limits:** at most 5,000 songs per share. A larger playlist offers to share its first 5,000 songs in display order.

### 10.7 The recipient's import (both clients)

**Opening.**
- Tap the file: the desktop hooks (section 6.1) or Android's `PurpleChatHooks.importSettings` (section 6.2).
- Or use **Open as playlist** from the message menu.
- Purple downloads the file in memory (at most 4 MiB), decodes it (at most 16 MiB) and validates it.

**Finding the songs.** Only the songs the recipient needs are looked up: those not already in their copy with an equal or newer stamp. The lookup is `messages.search` (`api.tl:2596`) in the chat that holds the file, with:
- an empty query;
- `inputMessagesFilterMusic` (verified, `api.tl:313`);
- `from_id` = the file's sender, except in broadcast channels;
- `max_id` = the file's message id;
- `top_msg_id` for a forum topic.

It pages backward until every needed document id is found, or until 50 pages (5,000 messages) have been scanned. The newest match wins (H20). Songs not found are left out and counted.

**The preview.** Core `PlanShareImport(library, share, resolved, copy map, now)` decides which of these the recipient sees:
- **New:** "Import playlist “Evening” from <sender>: 42 songs (2 not found in this chat)", with **Import** and **Cancel**.
- **Update**, when the id exists and is not purged: "Update “Evening” from <sender>", with a summary of songs added and removed by the file, a changed title or order when the file's register is newer, and "Your own changes stay", listing what the recipient changed later. The buttons are **Merge**, **Import as a new playlist** and **Cancel**.
- **Purged here:** "You deleted this playlist forever. Import it as a new one?" with **Import as new** and **Cancel**.
- **Up to date:** "“Evening” is already up to date with this file", with **Open**.
- **The account's own share:** the file's sender is this account and the id exists here. "This is your shared copy of “Evening”", with **Open**. Nothing is merged.
- **Newer format, Invalid, or too large:** "This playlist file was made by a newer Purple", or "This playlist file can't be read".
- **A stamp more than one day ahead of this device's clock:** only **Import as a new playlist** is offered.

**Staying linked, and detaching.** Nothing merges without a tap and this preview, so a recipient who wants no updates taps Cancel, or Import as a new playlist, on later files. There is no switch and no Unlink.

**The merge (ImportShare).**
- On the device that made copies, copy ids are first mapped to their originals (section 10.5).
- For the title, the order, and each song and tombstone, the import adopts the file's register only when its stamp is **larger** than the local one, or the local one is absent. On an equal stamp the local value stays.
- Adopted song values get `peer` = the chat where the file sits, as seen by the recipient, and `msg` = the message found.
- Imported stamps are copied, not created.
- If the local playlist was in Recently deleted, the import restores it with a fresh stamp. If it did not exist, the import creates it with `deleted=[false, fresh]`.
- Every adopted document id joins the playlist's exchanged set.
- The import changes the library, so the recipient's own sync publishes it.
- **Undo** restores every register the import wrote.

**What does not depend on order.** Importing two files in either order gives the same visible songs, title and order register. The source a song points to can differ, because ties keep the local value and each file rewrites sources to its own chat. Test C23 checks exactly that.

**Both sides' edits survive, at register level.** A song the sharer added, or removed later, reaches the recipient. A song the recipient added, or removed later, stays. Title and order are whole registers, so the later write wins, whoever made it. The update preview says which side wins.

**The result:** "Imported “Evening”: 40 songs, 2 not found in this chat · Open".

### 10.8 Re-sharing

- **The same device, the same chat:** section 10.3 offers "only the new songs" (the default) or "all again". The file always describes the whole playlist.
- **Another chat, or another device:** everything is sent, because there is no share history for that chat.
- **A recipient re-shares:** the file carries the same id, the recipient's register values and stamps, and sources rewritten to the new chat. Anyone holding the playlist merges it the same way, including the original sharer, whose device that made the copies maps them back.

### 10.9 What recipients on other clients see

- N audio messages from the sharer, with no "Forwarded from" header and no captions, in playlist order batch by batch (H14).
- Songs that were one album in the source and went in one batch may appear grouped as an album (H15).
- Protected songs appear as ordinary uploaded audio.
- Then one small file named "Evening.purpleplaylist", with the caption explaining that Purple Telegram can open it.

### 10.10 Large playlists, and limits stated in the docs

- At most 5,000 songs per share, at most 100 per request, and one request at a time.
- Flood waits are honoured with a visible countdown, and the job persists across restarts.
- A 2,000-song share from few chats takes at least 20 requests, and more when the source chat changes often. H16 measures real rates.
- Recipients' lookups are bounded at 50 pages.
- A re-share from another device duplicates protected copies for recipients, and an import on another of the sharer's devices adds copies beside originals.
- A recipient who joined a group after an earlier share, where the group hides old history from new members, may not find the earlier songs. **Send all again** fixes it.
- Songs the recipient deleted from the chat before importing show as "not found".
- A tombstone reaches recipients only from a device that exchanged that song.

## 11. Offline listening (P6 b and c)

Unchanged from revision 3, with lines updated.

**Download all**, in the playlist view, downloads every resolvable song of that playlist. It shows a progress row "Downloaded 120 of 140 · 2 failed · Retry" and a downloaded mark on each row.

**What Keep downloaded means, on both clients:** files fetched by **Download all** for a song of an unpurged playlist are exempt from Keep Media and the cache limits. A song that was merely played is not kept.

**Desktop.**
- With `keep_downloaded_p` on (the default), each song is saved into `<playlists>/files/<document id>.<extension>` with `DocumentData::save(origin, path)`.
- **Making the kept file the document's source.** `save` does not always record the target as the document's location (verified, `data/data_document.cpp:1227-1251`): with bytes in memory, it writes the file and records it (`:1243`); when a local file already exists elsewhere, for example in Downloads, it only copies that file and leaves the location unchanged (`:1250-1251`). So after each save, if `location(true)` does not name the kept file, Purple calls the public `setLocation` (verified, `data/data_document.h:167`) and `session().local().writeFileLocation(document->mediaKey(), ...)` (`mediaKey()` verified public, `:304`).
- Playback streams from the kept file, because `createStreamingLoader` prefers a local location (`data/data_document.cpp:1720`; H22).
- These files sit outside Telegram's media cache, so the cache size and time limits never evict them (the limits are at `storage/storage_account.cpp:185`). They are also outside its encryption (section 9).
- With the option off, Download all fills the managed cache as pinned-music download does (`docs/purple/pinned_music.md`), subject to those limits.
- A kept file is deleted once its document belongs to no unpurged playlist. Songs in Recently deleted stay until the purge.
- This needs no upstream hook.

**Android.**
- Download all reuses the pinned-music engine. It is first lifted out of the package-private UI classes into a shared Purple class, and it downloads to the path chat playback checks.
- **Keep downloaded** marks those files as kept forever:
  - one guarded call goes at the end of `CacheByChatsController.lookupFiles` (verified, `messenger/CacheByChatsController.java:146`);
  - upstream's automatic cleanup calls `lookupFiles` in both its age loop and its size loop, and skips any file whose `keepMedia` is `KEEP_MEDIA_FOREVER` (verified, `messenger/AutoDeleteMediaTask.java:91`, `:100`, `:161`);
  - `keepMedia` is package-private in `org.telegram.messenger`, so the Purple helper is a new Purple-owned file in that package, `messenger/PurplePlaylistKeep.java`. It marks every file whose path is in the account's kept list (H21).
- A manual **Clear cache** in Storage Usage still deletes kept files, because the person asked for it.

**Scope (choice P26).** One option covers every playlist, and there is no per-playlist switch. Its default is "keep forever", as the owner chose.

## 12. purple-core versus each client

**purple-core** holds every decision both clients must agree on. It gains new files and one field, and no shipped sync decision is edited.
- **Reused as is:** `purple/purple_peer_token.{h,cpp}` (section 3.1), the ownership predicate in `purple/purple_sync_chat.{h,cpp}`, `purple/purple_settings.{h,cpp}` for readers, `purple/purple_account_files.{h,cpp}` for paths.
- **Changed:** `AccountFiles` gains `playlists` (section 7.1); `purple_settings` gains the `[playlists]` and `[playback] private_listening_p` readers.
- `purple/purple_playlists.{h,cpp}`:
  - `QualifySong` with the protected-content flag;
  - format 1 parse, validate and canonical serialize, with partial playlists and `alt`;
  - the stamp rule;
  - the operations with their undo lists, including relink with fallbacks, AutoPurge with its conditions, CopyPlaylist and ImportShare;
  - join and contains;
  - the display view, and chat order for pinned selections;
  - the diff used for notices and import summaries;
  - limits and display-string truncation;
  - playlist ids from caller entropy;
  - parsing and serializing the local sync file, with the switch, the memo, `old_chats`, `due_seen` and `hidden`.
- `purple/purple_playlists_codec.{h,cpp}`: gzip encode and guarded decode (section 7.4).
- `purple/purple_playlists_sync.{h,cpp}`:
  - the record name, caption and tag, and `IsPlaylistRecordCandidate(meta, ownership)`;
  - `PlanPlaylistStep`, with the memo inputs, the publish rule, the purge conditions and the cover rule;
  - `PlanPlaylistChatMove`: which `old_chats` entries the new snapshot covers (section 8.12).
- `purple/purple_playlists_share.{h,cpp}`: building the share file from a snapshot, the send results, the copy map and the exchanged set; parsing it; and `PlanShareImport`.
- Docs: `docs/playlists.md` (format, merge, the cover rule, the publish rule, the automatic purge, the codec, the share file, the chat move, privacy and logging), plus a README entry.
- Tests: `tests/test_playlists.cpp` and `tests/test_playlists_sync.cpp`. The new sources join the explicit source list in `tests/run.sh`, which also gains `-lz`.

**Each client** owns:
- the files: location, atomic writes, `.bak`, the record cache, and unreadable files;
- mapping its message and document objects to `QualifySong` inputs, source values and ownership facts (through the existing facts mapper);
- resolution with fallbacks, and row states;
- the player integration;
- all UI and wording;
- the scheduler, the options' surgical writes, the logout hold and the session-end observer;
- Telegram I/O: the layer's lookup, the listing, the fetch by id, the reader's playlist mode, the generalized upload, deleting, forwarding, uploading copies, and the import search;
- downloads and keeping.

**Android's bridge.** A new stateless JNI bridge, `TMessagesProj/jni/purple_jni/playlists_bridge.cpp`, passes JSON and bytes. It can apply an operation, view a library, plan a step and a move, encode and decode, build and plan shares, and read and write the local sync file.
- It joins the `purplecore` source list in `TMessagesProj/jni/purple_jni/purple.cmake` (verified, `:10-21`), and `z` joins its link list (`:72-74`).
- A stateless bridge copies the whole library across JNI on every operation, and Java holds a JSON string as UTF-16, about twice its size. H19 measures one Add on a 24 MiB library through the bridge on the emulator. If that is too slow, the parsed library stays in native memory per account and only the operation crosses JNI.

## 13. Upstream edit surface

Every line below is marked `// Purple:` and has the rule's shape: one include, one guarded call, one early return, or one delegating row. The single guard condition on Android is called out. Each commit that adds one also updates that file's entry in `docs/purple/upstream_hooks.md`, and `purple/upstream_report.sh` measures it (tdesktop `AGENTS.md`).

**Desktop (`dev` at `38857d136c`):**
- `history/view/media/history_view_save_document_action.cpp:37-45`: one include and one call (Add, Add album, Open as playlist).
- `media/player/media_player_instance.cpp`: one include and three guarded early returns (`:611`, `:805`, `:824`), each first in its function.
- `media/player/media_player_panel.cpp`: one include and one early return in the `peer` lambda (`:241-265`).
- `media/player/media_player_widget.cpp`: one include and one guarded early return inside the label branch, after `:524-526` (P7).
- `window/window_main_menu.cpp:772-777`: one delegating menu row (P8).
- `menu/menu_item_download_files.cpp:237`, `:266`: one include and one guarded call in each overload (P5 c).
- `info/media/info_media_list_widget.cpp:1618-1623`: one include and one delegating call (P5 c).
- `data/data_file_click_handler.cpp:143`: one include and one guarded early return in `DocumentSaveClickHandler::onClickImpl` (share files).
- `data/data_document_resolver.cpp:239-241`: one include and one guarded early return (share files).
- `core/application.cpp:1041`: one guarded early return (P18).
- `media/player/media_player_listen_tracker.cpp:67-72`: one include and one early return (P20).
- `Telegram/cmake/purple.cmake`: the new Purple and core sources (not `Telegram/CMakeLists.txt`).
- `Telegram/Resources/langs/purple.strings`: the strings.

Plus one line that belongs to the save policy, not to playlists (commit T1): `info/media/info_media_provider.cpp:587-591`, a `|| Purple::SaveRestrictionsDisabled()` condition in `allowSaveFileAs`.

No new launch hook is needed: the scheduler starts from `Purple::StartAppServices()` (`core/application.cpp:354`) and registers the session-end observers. Auto-download exclusion stays inside `Purple::IsSyncRecordFileName`. Playlists add nothing to the receipt plumbing in `apiwrap`, `file_upload` or `localimageloader`.

**Android (`master` at `35a08c479`):**
- `ui/ChatActivity.java`:
  - one or two delegating cases in `processSelectedOption`, beside `:33695`;
  - three calls for chat multi-select (`:10392`, `:19493`, `:3855`), fewer if `purple.onMenuItem` (`:3948`) or `PurpleChatHooks` already covers them.

  The message menu (`:46782`) and the file tap (`:41945`) need no new line: they already call Purple.
- `ui/Components/SharedMediaLayout.java:2265-2271`: one call, plus possibly one in `updateForwardItem` (`:3904`).
- `ui/Components/AudioPlayerAlert.java`: nothing new; `PurpleMediaSave.addAudioOption` already runs at `:2876` and `:2952`.
- `messenger/MediaController.java`:
  - one import;
  - **one guard condition** (`:2874`, the rule's "smallest extension point");
  - two early returns (`:2924`, `:1904`);
  - P20's guard condition on the send line (`:7538`).
- `ui/DialogsActivity.java`: nothing new if the row goes inside `PurpleDialogsHooks` next to the preset row (`:13814`); otherwise one delegating row after `:13820`.
- `messenger/CacheByChatsController.java:146`: one guarded call (Keep downloaded).
- `messenger/MessagesController.java:16157`: one guarded early return (P18).
- `res/values/strings.xml`: new strings in the Purple block.
- `TMessagesProj/jni/purple_jni/purple.cmake`: the bridge source, and `z` in `purplecore`'s link list.

Everything else lives in Purple-owned files, including `PurpleSyncAutoDownload`'s new names and the `appDidLogout` observer. The desktop `docs/purple/playlists.md` names each hooked file and why, as `AGENTS.md` asks.

Compared with revision 3, Android loses four upstream lines (the message menu, the file tap and both player-sheet calls) and desktop's lines are unchanged in number.

## 14. Dependencies: what exists today, and what playlists build or wait for

**The sync layer (magic group).**
- **Exists:** the policy in core `sync-magic` (`purple_sync_chat_magic.{h,cpp}`, `docs/sync-chat-magic.md`), not merged into core master and so not in the gitlink this branch pins; the mark and carry removal in core `sync-magic-delete`, not merged; Android's implementation on `sync-magic` (lookup, creation, archive and mute, hiding through `PurpleGate.hiddenEverywhere`, the Sync chat screen, waits across restarts), not merged into Android master.
- **Not readable:** desktop's implementation. On desktop `dev`, settings sync uses Saved Messages, and the mark-based sync-chat code is present with every change switch off (`docs/purple/sync.md`, `docs/purple/sync_chat_wiring.md`).
- **Playlists wait** for each client's magic layer before that client's sync phase (C or E). They need the five interface points of section 8.2. If those points are missing when the layer lands, playlists add them to the layer's Purple files in their first sync commit.

**The user-control layer.**
- **Protected content (P13): exists, always on, no setting**, on both clients (section 3.3). Playlists read the existing policy function. The one remaining desktop gate (shared media, single song) is fixed by T1. G-5 no longer waits for a decision; it waits only for big-file upload parts.
- **Private listening (P20): does not exist.** Playlists build it (commits B0 and D0), or it lands first on its own (choice P35).
- **Kept deleted messages, edit history, view-once and timer media:** desktop has save-only timer media and an encrypted deleted-message archive; Android has archive lifecycle hooks (`PurpleArchiveLifecycle`, verified in use, `messenger/MessagesController.java:8293`). Playlists do not depend on them. Using archived copies as song fallbacks is a later item.

**Per-account settings (P10).** Exists as layered account files (section 8.8). Playlists add readers and defaults, nothing else.

**The refactor plans.**
- **Moving Purple code out of upstream files:** in progress as batches B1 to B19 with a hook registry (`docs/purple/upstream_hooks.md`); Android has `PurpleChatHooks`, `PurpleDialogsHooks` and policy classes. The owner's rule that the refactor lands before playlist code (round 1) is checked per hook site, not per file: at every site in section 13, the Purple lines already there are policy or hook calls (for example `Purple::AddSelfDestructDocumentSaveAction` at `history_view_save_document_action.cpp:42`, `purple.addSaveOptions` at `ChatActivity.java:46782`). Whether a whole file still holds unmoved Purple feature code elsewhere is the hook registry's record, not this design's; the first commit of phases B and D reads that file's registry entry and stops if its batch is still open. `main/main_account.cpp`'s two logout lines are avoided (section 8.10).
- **Sending records:** a Purple-owned upload and `sendMedia` exist on both clients for settings (section 8.2). Playlists generalize them (commits C-1 and E-1). The legacy receipt plumbing is still in upstream files; removing it is the refactor's business, and playlists do not touch it.
- **Share files:** nothing exists. Playlists build the format (core A4) and the send and open paths (phase G).

## 15. Phased plan

**What comes first:**
- Core phase A can start now, on a branch from core `sync-magic-delete` (or master once that merges), because A3 uses the ownership predicate that branch keeps.
- Desktop phases B and Android phase D are local only and can start once A1 and A2 are in the gitlink each client pins.
- Phase C waits for desktop's magic layer on `dev`. Phase E waits for Android `sync-magic` on master.
- Phase G-5 waits for big-file upload parts (G-0).
- Media and recording work may land in any order with playlists; section 5.5 lists the one shared function.
- Builds, host tests and emulator runs happen only between 00:00 and 10:30, as before.

**How commits work:**
- Each numbered item is one atomic commit with its docs, using an explicit pathspec.
- Core commits land first. Each client bumps its submodule in a commit of its own.
- A commit that adds an upstream hook updates `docs/purple/upstream_hooks.md` in the same commit.
- Push at the end of each phase.

**Effort.** Estimates are in focused working days for one agent with the owner reviewing, including tests and docs, excluding the owner's screen checks and the spike. They assume the dependencies above are in place when a phase starts.

### Phase T: small fixes that are not playlists (tdesktop and Android)

- **T1** (tdesktop) "Allow Save in protected shared media under the save policy": the `allowSaveFileAs` condition (section 3.3), with its `upstream_hooks.md` entry and a line in `docs/purple/user_control.md`. Half a day.
- **T2** (tdesktop, optional) "Fold the logout hooks into one Purple call" (section 8.10). Half a day. Only if the owner prefers it to the observer.

### Phase A: purple-core (8 to 10 days)

- **A1** "Add the playlist library model": `QualifySong`, format 1 with partials and fallbacks, stamps, the operations including AutoPurge and ImportShare, undo, join, contains, the view, chat order, limits, the local sync file, the `AccountFiles::playlists` field, the `[playlists]` and `[playback] private_listening_p` readers, `tests/run.sh`, and `docs/playlists.md`. 4 days.
- **A2** "Compress playlist records with a guarded gzip codec" (zlib, `-lz`, the bomb tests). 1 day.
- **A3** "Plan playlist sync steps with remembered records and the cover rule": `IsPlaylistRecordCandidate` over the shared ownership verdict, `PlanPlaylistStep`, `PlanPlaylistChatMove`. 2 to 3 days.
- **A4** "Build and import playlist share files". 1 to 2 days.
- **A5** "Describe playlists in the README". A quarter day.

**Acceptance for phase A.** All of it is automated, with thousands of seeded runs for the property tests:
- **C1** Join is commutative, associative and idempotent over random full and partial libraries, compared as canonical bytes.
- **C2** Three to five replicas end byte-identical under random operations, random delivery order, duplicated and delayed records, clocks skewed by up to a day, and two replicas sharing one copied file.
- **C3** An operation made after merging a write beats that write. A merge creates no stamp.
- **C4** Concurrent adds of different songs both survive. An album keeps album order.
- **C5** Delete against a concurrent add: the playlist is deleted, `hidden` lists it, and Restore shows both devices' songs.
- **C6** The order cases. An order register naming absent songs displays correctly.
- **C7** Undo writes back exactly the undo list, including for relinks with fallbacks and for imports.
- **C8** Random interleavings of publish, prune, merge and listing misses across three devices:
  - the join of the surviving records always equals the join of every record ever posted;
  - after two quiet steps per device, exactly one record remains;
  - a chat move in the middle (section 8.12) loses nothing, and leaves in the old chat only records no moving device had cached.
- **C9** The validator rejects unknown keys and empty playlists; stamps above 2^52 and a `since` above its stamp; bad or duplicate ids; an `alt` with more than 3 entries, with duplicates, or containing the primary; purged ids still present. It accepts partial playlists, and order ids absent from `songs`.
- **C10** No payload or share file that core can build contains `access_hash`, `file_reference` or a numeric self user id. Peer tokens built with the account's id always write `self`.
- **C11** Purge: a purged playlist never returns; a record holding it becomes covered; `hidden` carries the pre-merge playlist.
- **C12** A format 2 record or share file is neither merged, pruned nor imported. Nothing is serialized over a NewerFormat or Invalid local library.
- **C13** `IsPlaylistRecordCandidate`, the shipped `IsSyncHistoryCandidate` and the share-file detector are pairwise disjoint over: records, settings records and share files; a text mark carrying `#purplechatsync` (still posted by older builds); share files whose titles contain `#purplesync` or `#purpleplaylists`; forwarded copies of each; and each with ownership Other and Unknown.
- **C14** Golden files: fixed inputs give fixed canonical bytes. The Android bridge test runs the same files.
- **C15** The local sync file round-trips and keeps unknown keys.
- **C16** The codec round-trips. A 32 MiB + 1 expansion is Oversized, with peak output memory at most the cap plus one chunk. A second member, trailing bytes, a bad CRC or a missing magic is Invalid.
- **C17** The memo: a listing that misses a present record, confirmed by id, plans no publish and no prune; a record confirmed gone is dropped; a non-valid entry from an older classifier version is re-read; a transient failure or Unknown ownership is never memoized.
- **C18** The publish rule: a step publishes exactly when no present valid record's hash equals the merged library's, and never above the caps.
- **C19** Steady state: the same candidates, with a library equal to one of them, plan no download, no publish and no prune.
- **C20** AutoPurge: it purges at s + 30 days and not before; each of the four conditions, alone false, prevents it; a stale library whose record set lost its covering record (a memo record gone, nothing new) does not purge; a restore merged in the same step prevents it; a concurrent Restore and AutoPurge resolve to the purge, with `hidden` offering Keep a copy.
- **C21** Relink pushes the old primary into `alt`, deduplicates it, and caps it at 3. Remove drops `alt`.
- **C22** Share build: no `peer`, `msg`, `un` or `alt` appears; copies are substituted in `songs`, `order` and tombstones; left-out songs are absent from both; tombstones appear only for documents in the exchanged set; the caption builder emits no title and no `#`.
- **C23** ImportShare: adopts only larger stamps, and keeps local values on ties; restores a deleted playlist; sends a purged id to "new"; refuses stamps a day ahead; is idempotent when repeated; gives the same visible songs, title and order for two files imported in either order; maps copies to originals and adopts only tombstones for them; has exact undo.
- **C24** `QualifySong` with the protected-content flag on and off, and for a rich-message track, a Profile music item and a scheduled message.
- **C25** A local library file larger than 32 MiB loads and merges. A snapshot above the caps is refused with a reason.

### Phase B: desktop playlists, local only (8 to 10 days)

- **B0** "Keep music listening private" (P20): the tracker hook, the `[playback]` row in Media playback, docs. Half a day. Lands first, or alone (P35).
- **B1** "Update purple-core for playlists" (the submodule bump and `purple.cmake` sources).
- **B2** "Store each account's playlists in its account directory".
- **B3** "Resolve playlist songs, falling back to earlier copies".
- **B4** "Add songs, albums and selections to playlists from song menus" (the save-action hook and the three selection hooks).
- **B5** "Show and edit playlists from the main menu" (the boxes, the painted list, Recently deleted with dates, recent-use order, and the `menu_item_p` row).
- **B6** "Play a playlist in its own order" (the four player hooks).
- **B7** "Save pinned songs as a playlist in chat order".
- **B8** "Download playlists and keep them past the cache limits".
- **B9** "Document desktop playlists".

**Acceptance** runs on the disposable account, in an isolated instance, at night. The owner verifies anything that needs the screen.
- **D1** Adding songs: from a chat, a topic, Saved Messages, a channel, a migrated group's old half, shared media, the hover list, and multi-select in a chat and in shared media; from a protected chat everywhere (after T1 for shared media); a forwarded copy relinks in place, and the old copy becomes a fallback; record whether the playing song reports `loading()`.
- **D2** Playing: songs play in order, including next, previous, auto-advance, media keys and OS controls; the modes behave as named; leaving playlist mode, the hover panel, and the track-name click behave as specified; a voice message in the middle pauses and resumes the playlist (with F3 on when `media` has landed); F9's back after the last song replays it and keeps playlist mode, and back while the next song is resolving does not start that song over the replay; count upstream's shuffle-list requests; a 2,000-song playlist opens and drags smoothly.
- **D3** Deleting a source makes the row fall back to an earlier copy when one exists, and shows Deleted otherwise. **Use this copy** relinks it.
- **D4** After a restart the library is intact and channel songs resolve (H2). A NewerFormat file stays byte-identical.
- **D5** Save as playlist uses chat order and reports skipped files.
- **D6** Download all with Keep downloaded on: plays offline after a restart (H22); survives "Clear cache" in Local Storage; after a song had also been saved to Downloads, deleting the Downloads copy and going offline still plays it. With the option off, the files follow the cache limits.
- **D7** Private listening on: no `messages.reportMusicListen` for chat or playlist playback (H4).

### Phase C: desktop sync (6 to 8 days, after desktop's magic layer is on `dev`)

Before this phase, run the spike on the disposable account: H1, H3, H8, H9, H11, H19, H26, H27, H29 and H30.

- **C-1** "Let the sync upload send any Purple record": generalize `SyncConfigUpload` and the send and edit requests (name, MIME type, caption, attributes, silent), with settings passing today's values and its tests unchanged. 1 day.
- **C-2** "Find and read playlist records in the sync chat": the history listing and the Saved Messages search with full paging, the fetch by id, the reader's playlist mode, the codec, and the auto-download names. 1 to 2 days.
- **C-3** "Sync playlists through the sync chat": the step with the memo and cache, the publish rule, pruning, the automatic purge, the scheduler, the options, the notices, the sync line and the first-run bulletin. 2 to 3 days.
- **C-4** "Move playlist records when the sync chat changes" (section 8.12). Half a day.
- **C-5** "Keep or sync playlists at logout": the hold, the dialogs and the session-end observer. 1 day.
- **C-6** "Describe playlist sync". `playlists.md` gains its sync section. In `docs/purple/account_sync_plan.md`, the library-stream lines and the "Playlists later" section are replaced with a pointer to this design (`:138`, `:158-159`, `:549`, `:555-567`, `:612`, `:762-785`). `docs/purple/sync.md`'s "Library/playlist publishing remains disabled" sentence (`:730`) is updated. Half a day.

**Acceptance:**
- **S1** The first step publishes a snapshot into the magic group. An edit publishes within 60 s. Ten edits spread over 9 minutes publish once, within 10 minutes of the first edit.
- **S2** A check with nothing new sends exactly one playlist request (one history page), counted from Purple's own counters, and writes nothing. The layer's lookup adds no request while its full info is fresh.
- **S3** Three records are left alone: a hand-posted malformed record is reported and never deleted; a forwarded copy of a valid record is ignored; a record over 10 MiB compressed is reported as oversized without being downloaded.
- **S4** A second instance with a copied `tdata` and account directory: after both have stepped twice, one record remains.
- **S5** A delete that failed (the network was cut after the publish) is retried and succeeds at the next step.
- **S6** Purges: a 30-day purge through a test-only hook in the core tests; a purge by a second instance; an instance started from a copy made before a Restore does not purge the restored playlist.
- **S7** Logout: with sync on, the files are deleted only after the logout completes; cancelling at upstream's download prompt keeps them; with sync off, the warning appears; offline with changes waiting, the retry dialog appears; with the layer waiting, the sync-settings dialog appears; a forced logout deletes the files.
- **S8** Two instances that each created a magic group converge on the older one; the newer group's playlist records are deleted by the instance that cached them; the library is complete on both.
- **S9** Settings sync is unaffected: with playlist records present, the settings Check, automatic settings sync and the layer's chat move reach their normal verdicts.

### Phase D: Android playlists, local only (10 to 12 days)

- **D-0** "Keep music listening private" (P20): the send-line guard, the settings row. Half a day.
- **D-1** "Bridge the playlist library to Java".
- **D-2** "Keep a Purple playlist's order in the music player" (the four `MediaController` edits and the `endReached` params).
- **D-3** "Store playlists and resolve their songs".
- **D-4** "Add songs, albums and selections from the chat, shared media and the player" (inside `PurpleChatHooks` and `PurpleMediaSave`, plus the selection hooks).
- **D-5** "Show, edit and play playlists from the chat list menu and Settings".
- **D-6** "Save pinned songs as a playlist and play the pinned list in chat order".
- **D-7** "Download playlists and keep their files past Keep Media".
- **D-8** "Document Android playlists".

**Acceptance** runs on the local arm64 emulator at night.
- **A1** D1's cases, from the chat menu, the chat and shared-media action bars, and the player sheet, including the sheet's Add to playlist for a protected playing song. The picker over the sheet leaves the sheet open.
- **A2** Playing: Play prepares, then the sheet lists the playlist in order with the "Music" title and chat lines on each row; next, previous, the notification and the lock screen follow it (H5); Repeat One and stop-at-end work, and no chat music loads (H24); songs with equal ids from different chats both play; starting on the playing song resumes it; a song that fails to resolve is skipped; a voice message in the middle pauses the playlist, which then resumes in order on its own; with `recording` landed, a voice recording pauses music and Play resumes in order.
- **A3** D3's cases.
- **A4** After a force-stop, the library is intact and the queue is gone.
- **A5** The pinned list of a migrated group plays, and saves, in chat order.
- **A6** Kept files survive the age cleanup and the size cleanup (H21). A song that was only played is not kept.
- **A7** D7's case on Android.

### Phase E: Android sync (5 to 7 days, after Android `sync-magic` is on master)

- **E-1** "Let the sync upload send any Purple record" (`PurpleSyncChatUpload` and the part sender, generalized).
- **E-2** "Find and read playlist records in the sync chat", plus the new names in `PurpleSyncAutoDownload`.
- **E-3** "Sync playlists through the sync chat".
- **E-4** "Move playlist records when the sync chat changes".
- **E-5** "Keep or sync playlists at logout".
- **E-6** "Document Android playlist sync".

Before this phase, run H7, H11, H26, H27, H29 and H30 on the emulator. Acceptance: S1 to S9 on the emulator.

### Phase F: cross-client acceptance (2 to 3 days, no code)

Results go into the docs, in one commit per repository.
- **X1** Create on desktop and sync automatically. Android gets the same playlist in the same order, and it plays. DM, basic-group and channel songs resolve on a session that never opened those chats (H1).
- **X2** Both devices offline: each adds a different song and renames the playlist. After reconnecting, both songs are on both devices, with one title.
- **X3** Deletion, Restore, Delete forever with Keep a copy (also after a restart), and the 30-day purge.
- **X4** After quiet steps, the magic group holds exactly one playlist record.
- **X5** A corrupted record is reported on both devices and never deleted. Settings checks are unaffected.
- **X6** Download every record posted in X1 to X4 and scan it for access hashes, file references, the user id, paths, device names and settings content. After a Remove, a Delete forever and the next sync, the removed song's chat and the purged title appear in no record. Scan both clients' logs for titles, peer tokens and document ids.
- **X7** A private-channel song shows No access after the channel is left, and falls back when a fallback exists.
- **X8** A build without playlists checks settings while records exist, and reaches its normal verdict.
- **X9** Clearing Android's app data restores the library from the magic group.
- **X10** Steady state: an hour of idle app starts sends one history page per check and no downloads.
- **X11** One device bound by hand to Saved Messages and one in the magic group: each publishes where its layer binds it, and the owner is told that the two devices do not share playlists until both use the same chat (section 8.12 and choice P38).

### Phase G: sharing (8 to 10 days)

- **G-0** "Upload big files through the sync upload" (`upload.saveBigFilePart`, both clients, one commit each). 1 to 2 days.
- **G-1** "Share playlists by forwarding their songs and a playlist file" (desktop).
- **G-2** "Open shared playlist files as playlists" (desktop, both tap hooks).
- **G-3** and **G-4**: the same two on Android.
- **G-5** "Upload copies of protected songs when sharing, after asking" (both clients). Until it lands, G-1 and G-3 skip protected songs and say so.
- **G-6** "Document sharing".

**Acceptance**, on the disposable account, with a second account or a bot where one is needed:
- **SH1** Share a 30-song playlist from three chats to a DM. There is no header or caption, the order holds (H14), there is one notification, and the file comes last.
- **SH2** The recipient imports it. All songs are found (H20) and play, and the file holds no source chat or message id (a scan).
- **SH3** The sharer adds two songs, removes one and renames the playlist, then re-shares "only new". The recipient's Merge adds two and removes one. The title follows the stamp, and the recipient's own earlier addition survives.
- **SH4** The recipient re-shares to the sharer: on the sharer's device that made the copies, the merge adds no protected duplicates; on the sharer's other device, the duplicates appear, as documented.
- **SH5** Protected songs: the dialog's counts and sizes are right, Upload copies yields unprotected copies (H18), and Skip leaves them out.
- **SH6** A 500-song share under flood waits resumes after a forced quit (H16, H17).
- **SH7** A playlist titled "#purplesync test", shared to Saved Messages, leaves the next settings Check at its normal verdict.
- **SH8** Slow-mode, paid-message, no-rights chats and the account's own sync chat are refused in the chooser.
- **SH9** A non-Purple client (official desktop) shows plain songs and a harmless file.
- **SH10** Import on two recipient devices before they sync converges to one playlist.
- **SH11** On desktop, a first tap on an undownloaded share file opens the import, and nothing appears in Downloads.

**Total**: about 50 to 60 working days across the three repositories, plus T1 and T2, plus the spike and the owner's checks. Phases A, B and D can run while the sync layers land, so the critical path is A, then C and E behind their layers, then G.

### Later (each with a trigger)

- **Delta records.** Trigger: H19 or real use shows snapshot traffic matters (P30).
- **Edit in place** for this device's own record (P36). Trigger: the owner prefers fewer messages and deletes.
- **One listing for settings and playlists.** Trigger: request counts matter.
- **Remove downloads** per playlist. Trigger: the owner asks.
- **Following share updates automatically.** Trigger: the owner asks. It needs a search per followed chat.
- **The Android Auto browse tree.**
- **A re-assert operation** that replays a `.bak` as new writes.
- **Songs from timer messages, rich messages and the deleted-message archive** as sources or fallbacks. Trigger: the owner asks, once the archive has a stable read API on both clients.
- **A synced copy map.** Trigger: duplicates from cross-device protected re-shares bother the owner.
- **Encrypted playlist files** (P37).

## 16. Risks

- **Deleting messages is new for playlists.** A bug in contains could delete unmerged data. Mitigations: exact canonical comparison in core; pruning only valid format 1 records that a record read in the same step covers; the proof in section 8.5.1, backed by C8; every device's library still holds its own data and republishes it at its next step; H3 checked before phase C.
- **Two record families in one chat.** A playlist bug could delete a settings record, or the other way round. Each family deletes only message ids its own predicate accepted and its own memo or ledger holds, and C13 proves the predicates disjoint. S9 checks settings sync with playlists active.
- **The memo could hide a record.** It is keyed by message id and document id, a miss is confirmed by id, non-valid statuses are re-read after an upgrade, and it is cleared on Start over and on a chat move. C17, C19 and S2 test it.
- **Concurrent snapshots** from two devices cost one extra upload each, and the next step merges and prunes them.
- **Automatic purge on stale or lagging data.** Guarded by running after the merge, complete discovery, the newer-format check, the request-proven clock, and the 15-minute second sighting (section 7.9). C20 and S6 test it.
- **The layer is not on desktop `dev` yet,** and Android's is unmerged. Phases C and E slip with them. Phases A, B and D do not.
- **Default-on sync creating a group.** A person who never asked for sync gets a hidden group on their account. P33 decides; the bulletin discloses it either way.
- **Devices bound to different chats** (one by hand to Saved Messages, one magic) do not share playlists. Both keep working, but the person may not notice. The sync line names the chat on each device, and P38 decides whether to warn.
- **gzip bombs**, from records or share files. The streaming guard (C16) handles them.
- **Large libraries on phones.** H19 decides whether the stateless bridge stands.
- **Upstream merges** touch the files listed in section 13. Each hook is one line, `MediaController.java` keeps a single guard condition, and the hook registry tracks them. The media branches share `moveInPlaylist` (section 5.5).
- **Search reliability** (H10, H20) now matters only for Saved Messages and for share imports.
- **Sharing spam and flood.** Large shares can take minutes, and Telegram may throttle. The confirmation warns, and the job persists.
- **Protected content.** Copies spread content its sender protected. The policy is always on today with no switch (P34). The dialog says the copies are unprotected. Telegram's guidance and the api_id terms are stated in section 10.5.
- **Hostile share files** can only affect the one playlist they name. Every import is previewed and undoable, and future stamps are refused.
- **Songs rot.** Channels delete posts. Fallbacks and relinks soften this.
- **Plain local files.** The library and kept audio sit outside Telegram's passcode-encrypted storage, as settings files do. The docs say so, and P37 offers encryption.

## 17. Deliberately left out

- Live receive (merging on message arrival), edit in place, and automatic share updates.
- Delta records in v1 (P30).
- Any link to settings sync's spaces, installs, sequences, clone checks, ledgers and device lists, and the reserved library stream (P31).
- Any mark, redirect record, global search or chat with other readers.
- Collecting removed-song tombstones beyond purges.
- Per-playlist shuffle or repeat, manual list order, covers, descriptions, folders and smart playlists.
- The same song twice in one playlist.
- Audio without a music attribute, and secret-chat, scheduled, story, poll-media, rich-message, Profile music and timer songs.
- Automatic relinking, and writing a working fallback back as the primary.
- Import or export to upstream Profile music.
- An Info-layer playlist section, and the Android Auto tree.
- A per-playlist Keep downloaded switch (P26), and Remove downloads.
- A desktop selection top-bar button (P24).
- Unlink and a follow switch for shared playlists.
- A configurable check interval.
- **To Playlist** inside Android's Save To submenu (section 6.2).

## 18. Alternatives considered and rejected

- **Playlist records in the reserved library stream of settings sync** (section 8.1). Kept open as P31.
- **A separate Magic Purple Playlists group.** It would isolate the two families completely, but it doubles the lookup, the creation, the waits, the hiding and the person's hidden chats, for a separation the predicates already give. The brief asked for a strong reason to split; none was found.
- **A tag search in the magic group.** History is complete and has no index lag; search adds lag for no saving in a near-empty chat. Kept only for Saved Messages.
- **Reading the old chat on a move** (revision 3's section 8.13 and the carry). Every device's library already holds what it published, so the read gains nothing, and the carry it used is deleted.
- **Each playlist as a private channel.** Order is post order, protected songs cannot be forwarded, and every playlist is one more chat.
- **Playlists as a section of `settings.toml`**, **upstream Profile music**, **Saved Messages tags**, **fake history items**, **Android negative-id queue items**, **mutating `getPlaylist()` directly**, and **one record per playlist**: rejected for the first design's reasons.
- **Delta records with a dirty set and compaction triggers** (revision 2). More machinery than the decisions asked for.
- **A cloud-view library diffed against the local one.** It needs another large file and a recompute after deletions.
- **Automatic purge before the merge, or from the Playlists screen.** A stale device could purge a playlist restored weeks earlier.
- **Purging only when the record carrying the winning deleted register was read in this step.** A stale device's own fresh snapshot satisfies it.
- **Desktop `unixtime::updates()` as the clock gate.** It never fires on a correct clock.
- **A file-backed desktop download for records above 10 MiB.** It needs a Purple download loop for a size no realistic library reaches. P29 (b).
- **The existing receipt-threaded send paths** for records. They live in upstream files, and Android's cannot send silently.
- **Deleting files in the logout hook.** Upstream's later prompts can cancel the logout.
- **Bypass hooks around each upstream protection gate.** The save and copy policies already open them at the root; the one closed gate is fixed in the policy (T1).
- **Resolving ahead during playback on Android.** Unresolved songs cannot sit in upstream's queue.
- **A synced copy map.** It adds a field to the source value, for a rare round trip.
- **A separate share id.** Concurrent imports on two recipient devices would leave two playlists with one share id.
- **Merging fallbacks as a bounded set.** It breaks associativity against removals.
- **A snapshot or delta kind field in records.** Correctness never needs it.
- **Destination message ids in share files.** In private chats and basic groups, each side numbers messages separately.
- **`messages.sendMedia` with the existing document instead of forwarding.** It works for unprotected media only, one document per request.
- **Compressing the local library file.** It saves little and makes the file uninspectable.
- **An automatic copy on purge.** Without causal metadata it would bring back deliberate deletions.
- **An Android `clearPlaylist()` hook** for ending playlist mode. Identity comparison against the public `getPlaylist()` needs no edit.
- **Every tombstone in every share file.** It reveals document ids of songs that never left the device.
- **Library files under `tdata/`.** The account directory now exists and is shared by both clients.

## 19. Hypotheses to verify

Each stays unverified until run. Results go into `docs/purple/playlists.md`, without account identifiers.

- **H1** `messages.getMessages` with bare ids returns private-chat and basic-group messages on a session that never opened those chats.
- **H2** Desktop resolves a channel song after a restart, once the dialog list loads. `contacts.resolveUsername` resolves an unjoined public channel. A left private channel returns `CHANNEL_PRIVATE`.
- **H3** A session can delete a playlist record another session of the account posted: in the magic group with `channels.deleteMessages`; in Saved Messages with `messages.deleteMessages` and `revoke`; in an old magic group after a rebind. The deletion reaches the other sessions on both clients.
- **H4** With private listening on, neither client sends `messages.reportMusicListen` for playlist or chat playback. Turning it off in the middle of a song reports nothing for the earlier part. With it off, playlist playback reports as chat playback does.
- **H5** On Android, the player sheet, the notification and the lock-screen controls follow the ordered queue, with shuffle on and off.
- **H6** Retired in revision 3.
- **H7** Android's `getCurrentTimeMillis()` includes the server time difference.
- **H8** Whether official clients and Purple Android auto-download a 0.1 to 10 MiB `Purple playlists.json.gz` when the magic group is opened, at default settings.
- **H9** Desktop streams a song whose message was loaded only through `requestMessageData`, and refreshes its file reference after it expires.
- **H10** Saved Messages only: `messages.search` with `q = "#purpleplaylists"` and the document filter returns every record within a minute of posting. Record whether short pages occur before the end. Test the file-name query too.
- **H11** The ownership mapper gives `Own` for this account's playlist records in the magic group, as seen from every session of the account, and never `Own` for a forwarded copy or another account's message (now checked through core's predicate, not the bare `out` flag).
- **H12** Retired: in the magic group the account is the only member, so `from_id` filtering is not needed.
- **H13** Saved Messages only: `q = "#purpleplaylists"` returns only messages carrying that exact tag, and does not return `#purplesync` records.
- **H14** `messages.forwardMessages` accepts 100 ids and refuses 101 (record the error); the destination order follows the id vector; `drop_author` with `drop_media_captions` leaves no header and no caption on music; `silent` suppresses notifications.
- **H15** Album songs forwarded together in one batch stay grouped. Songs from different albums or chats do not merge into a group.
- **H16** Flood behaviour when sharing 500 and 2,000 songs to a DM and to a group: requests before the first FLOOD_WAIT, typical waits, total time.
- **H17** Re-sending a forward batch with the same random ids after a lost response creates no duplicate.
- **H18** A re-uploaded protected song keeps title, performer, duration and cover from its tags; has no `noforwards`; is matched by the music filter; leaves no copy in desktop's Downloads.
- **H19** Parse, join and serialize time and peak memory for 7,200-entry, 24 MiB and 32 MiB libraries, in core's host tests and through the JNI bridge on the emulator, including one Add on a 24 MiB library through the stateless bridge. A 7,200-entry step stays under one second.
- **H20** The import search finds the forwarded songs in DMs, basic groups, supergroups, forum topics and channels.
- **H21** Android's age and size cleanups skip files marked through the `lookupFiles` hook.
- **H22** Desktop plays a Download-all song from the Purple folder after a restart with the network off, including after an earlier Downloads copy was deleted.
- **H23** Retired in revision 3.
- **H24** Android `setPlaylist` with `loadMusic = true` and Purple's `endReached` params loads no chat music, sends no `loadMoreMusic` request, honours Repeat One and stop-at-end, and shuffles.
- **H25** Retired: there is no marker and no global search.
- **H26** Desktop: `account->sessionChanges()` fires `nullptr` with `!Core::Quitting()` on a user logout, a forced logout and a forgotten-passcode logout, and never on quit. Android: `appDidLogout` fires for `performLogout` types 0, 1 and 2 on that account's `NotificationCenter`.
- **H27** The generalized upload, small parts for a record and big-file parts for an audio file over 10 MB, followed by `messages.sendMedia` with `silent` and a Purple random id, with the returned Updates applied through `applyUpdates` or `processUpdates`, shows the message normally on the sending device, yields its message id and document id, notifies no one, and leaves no copy in desktop Downloads or Android's Telegram Files.
- **H28** On desktop, `DocumentSaveClickHandler::onClickImpl` is the tap path for an undownloaded share file in a chat, a topic, shared media's Files tab and search results.
- **H29** The scheduler can list, step and upload for an account that is not active on desktop, or not selected on Android, without that account's UI, and the layer's lookup works for it.
- **H30** In the magic group, one `messages.getHistory` page of 100 lists every record after a week of normal use with settings and playlist sync both automatic; and a record posted by another session appears in the next page read with no delay.

## 20. Sources

- **purple-core** `d51512e` (master, the pinned gitlink), `sync-magic` `bf5da4c`, `sync-magic-delete` `7905290`:
  - `docs/sync-chat-magic.md` (sync-magic), `docs/sync-chat-core.md` (both), `docs/settings-layering.md`, `docs/account-local-choice.md`, `README.md` (35-60, 150-180, 280-330);
  - `purple/purple_sync_chat_magic.h` (25, 41-226), `purple/purple_sync_inventory.{h,cpp}` (`h:27`, `cpp:40-120`), `purple/purple_sync_local_state.h` (20-80, 240-256), `purple/purple_account_files.{h,cpp}` (`h:20-60`, `cpp:40-75`), `purple/purple_peer_token.h`;
  - `git diff --stat` of `d51512e..sync-magic` and `sync-magic..sync-magic-delete`, and the `sync-magic-delete` commit message.
- **tdesktop** `dev` `38857d136c`:
  - `AGENTS.md` ("Keeping upstream merges easy", 221-246);
  - `docs/purple/user_control.md`, `sync.md` (1-60, 580-746), `account_routing.md` (1-40), `account_sync_plan.md` (138-160, 549-612, 762-785), `upstream_hooks.md` (1-40), `config.md`;
  - `purple/purple_user_control.h` (36), `purple/purple_copy.h` (15), `purple/purple_sync_config_upload.{h,cpp}` (36-37, 170-190), `purple/purple_sync_chat_write.cpp` (10-40), `purple/purple_sync_candidate_reader.cpp` (21, 248-270), `purple/purple_sync.cpp` (707, 740, 769), `purple/purple_last_seen.cpp` (60-66), `purple/purple_sync_account_setup.cpp` (114), `purple/purple_pinned_music.cpp` (169);
  - `history/history_inner_widget.cpp` (3185-3205, 3498-3512, 3806-3816, 3993-4040), `history/history_item.cpp` (3442-3455), `history/history_item.h` (490), `history/view/history_view_list_widget.cpp` (2131-2180), `history/view/history_view_context_menu.cpp` (1215-1232, 1415), `history/view/media/history_view_save_document_action.cpp` (30-50, 118-140), `history/view/media/history_view_document.cpp` (1317, 1482-1492), `history/view/history_view_top_bar_widget.h` (237);
  - `info/media/info_media_list_widget.cpp` (1528-1562, 1612-1625), `info/media/info_media_provider.cpp` (78-92, 587-591), `menu/menu_item_download_files.cpp` (237, 266);
  - `media/player/media_player_instance.cpp` (53-80, 147-175, 340-365, 475-500, 596-650, 805-845, 1032, 1221, 1235, 1471), `media/player/media_player_panel.cpp` (236-270), `media/player/media_player_widget.cpp` (522-536), `media/player/media_player_listen_tracker.cpp` (52-99), `media/player/media_player_instance.h` (58-70);
  - `data/data_document.h` (167, 201, 304), `data/data_document.cpp` (1227-1251, 1440, 1720), `data/data_document_media.cpp` (305-320), `data/data_document_resolver.cpp` (232-250), `data/data_file_click_handler.cpp` (74, 143-145), `data/data_peer.h` (336), `data/data_chat_participant_status.h` (62, 64), `core/file_location.cpp` (78);
  - `core/application.cpp` (354, 1041-1062, 2171), `main/main_account.h` (75, 80, 123-126), `main/main_account.cpp` (58-64, 218-232, 536-569), `main/main_domain.h` (77);
  - `storage/file_download.h` (30), `storage/file_download.cpp` (126), `storage/storage_account.cpp` (185, 1884), `mtproto/session_private.cpp` (1636, 1646, 2175-2178);
  - `apiwrap.h` (161, 377-402), `apiwrap.cpp` (534, 644, 2844, 2855, 3902, 3985, 3988, 4346-4373), `boxes/share_box.cpp` (1970), `window/window_peer_menu.h` (198), `window/window_main_menu.cpp` (765-780), `window/window_session_controller.cpp` (1597, 1626);
  - `mtproto/scheme/api.tl` (309, 313, 2593, 2595, 2596, 2599, 2603, 2604, 2649, 2839, 2901, 2903, 2908);
  - `Telegram/CMakeLists.txt` (100), `Telegram/cmake/purple.cmake`.
- **tdesktop** `media` `884225999c` and `recording` `8f792fae58`: `git diff origin/dev...` of `media/player/media_player_instance.cpp` and `media_player_widget.cpp`; `docs/purple/media_design.md`, `media_playback.md`, `media-recording-design.md`, `media_recording.md`.
- **Android** `master` `35a08c479`:
  - `messenger/MediaController.java` (1523, 1714, 1904-1916, 2482, 2735, 2760-2770, 2865-2924, 2975, 3380, 3641-3680, 3901, 4290, 4429, 7344, 7367, 7373-7420, 7520-7545);
  - `messenger/MessagesController.java` (6723, 7257, 8293, 16157, 16175, 16207, 17903), `messenger/SendMessagesHelper.java` (9949), `messenger/CacheByChatsController.java` (146), `messenger/AutoDeleteMediaTask.java` (49, 91, 100, 161), `messenger/ChatObject.java` (2189, 2196), `messenger/MessageObject.java` (12253), `tgnet/ConnectionsManager.java` (304);
  - `messenger/purple/PurpleContentPolicy.java`, `PurpleCopyPolicy.java`, `PurpleSyncAutoDownload.java`, `PurpleSyncChatUpload.java`, `PurpleSyncTelegramClient.java` (271);
  - `ui/ChatActivity.java` (1663, 3855, 3948, 10392, 19369-19493, 33628-33700, 41944-41945, 42030, 46770-46782), `ui/PurpleChatHooks.java` (54-65), `ui/Components/AudioPlayerAlert.java` (1345-1376, 2539, 2862-2956), `ui/Cells/AudioPlayerCell.java` (86, 112), `ui/Cells/ChatMessageCell.java` (17872), `ui/Components/SharedMediaLayout.java` (2236, 2265-2271, 3904), `ui/DialogsActivity.java` (586, 2844, 13718, 13800-13830), `ui/LaunchActivity.java` (592), `ui/PurplePinnedMusic.java` (378), `ui/PurplePinnedMusicList.java` (303);
  - `TMessagesProj/jni/CMakeLists.txt` (320, 337), `TMessagesProj/jni/purple_jni/purple.cmake` (5-21, 72-81), `gradle.properties` (17).
- **Android** `sync-magic` `0f0ef0701`: `docs/sync-chat.md`; `messenger/purple/PurpleSyncMagicChat.java` (56-103, 153-177, 287-307, 398-421, 580-583), `PurpleSyncTelegramClient.java` (100-175), `PurpleSyncChatUpload.java` (1-60), `PurpleSyncScanner.java` (13, 79), `PurpleSyncChatRequests.java` (44-50); the commit list of `master..sync-magic`.
- **Android** `media` `39ad300e1` and `recording` `f1eef62b5`: `git diff master...` of `messenger/MediaController.java` and `messenger/MusicPlayerService.java`.
- **Telegram documentation**, as read for revision 3 on 2026-09-30: `api/search`, `method/messages.searchGlobal`, `mtproto/description`, `api/files`, `method/messages.forwardMessages`, `method/messages.search`, `api/errors`, `api/content-protection`, `api/peers`, `api/updates`, `api/file-references`.
- **Decisions:** `owner-answers-r1.md`, `owner-answers-r2.md` (with round 2b), and revision 3 with its consistency amendments.

## 21. The owner's settled answers, and their status today

Every settled answer stands, except where the code has overtaken it. Those are said plainly.

- **P1, P2, P12, P14, P17**: settled at their defaults in revision 3. Nothing in today's code affects them.
- **P3 (d)**, two-level deletion with a 30-day automatic purge: stands (sections 4.1, 7.9).
- **P4**, automatic coalesced sync with checks at most every 15 minutes, configurable: stands, now sharing triggers with automatic settings sync (section 8.6).
- **P5 (c)**, Add to playlist in the multi-select bars: stands for Android. On desktop the bar still has no hook point, so the entry stays in the selection menus (P24).
- **P6 (b) and (c)**, Download all and Keep downloaded, default keep forever: stands (section 11).
- **P7**, track-name click opens the playlist: stands (section 5.2).
- **P8 (b)**, a navigation drawer item with a setting, default shown: stands. On Android the item is in the chat list's main menu (P25). Revision 3's extra **To Playlist** inside Android's Save To is dropped in favour of the Purple row one level up (section 6.2).
- **P9**, a user-chosen sync chat that may have other readers, with warnings, found by a tag: **overtaken by the code.** The layer now allows only the magic group, Saved Messages, or a chat with no other member or admin, and finds its chat in the local list, not by a tag. Playlists follow the layer. If the owner still wants shared sync chats, that is a change to the sync layer, not to playlists.
- **P10**, per-account overrides of global keys: **implemented differently.** Per-account files layered over the global file, instead of sections inside one file. The behaviour the owner asked for (account values override global ones, settings sync carries them) holds (section 8.8).
- **P11**, relink with earlier links kept as fallbacks: stands (sections 3.4, 4.1).
- **P13**, a new setting, on by default, allowing all actions in protected chats, with protected songs fully usable: **partly implemented.** All local actions are allowed on both clients, always, with **no setting** (section 3.3). Playlists can use protected songs today. The missing switch is P34. Forwarding stays impossible server-side, so sharing re-uploads (section 10.5).
- **P15**, relax the cap and gzip the record: stands (P29 for the numbers). The merge happens only when another device's new record appears (section 8.5).
- **P16 (a)**, the naming next to Profile music: stands.
- **P18**, a last sync before deleting at logout; a warning and a choice when sync is off: stands (section 8.10).
- **P19 (b)**, recently used first: stands.
- **P20 (b)**, a switch, default private listening: stands, but **nothing implements it yet** in either client (section 5.1, P35).
- **P21 (b)**, playlist sync on by default: stands, with the new question of who creates the magic group (P33).
- **Sharing (round 2)**: stands, with the playlist id as the share identity (section 10.2), and re-uploads of protected songs behind the dialog (round 2b), no longer blocked.
- **The refactor first (round 1)**: met for every file playlists touch (section 14).

## 22. Product choices still open

Each lists its options, the trade-offs and the default this design builds. P22 and P28 are retired, because the magic group removed the questions they asked.

- **P23 Re-sharing to a chat this device shared to before.**
  - (a) Send only the new songs, with "Send all again" in the dialog. Fewer messages; a recipient who lost the earlier ones must ask for all again.
  - (b) Always send every song. Simple and robust; floods the chat on every update.
  - **Default (a).**
- **P24 Desktop multi-select entry (a departure from P5's "bars").**
  - (a) The selection context menus in chats, sections and shared media, with one-line hooks.
  - (b) Also a button in the selection bars, which needs a member, its wiring, geometry, visibility and counter lines across `history_view_top_bar_widget.{h,cpp}` and the shared-media top bar, about fifteen upstream lines.
  - **Default (a)**, because (b) breaks the hook rule.
- **P25 Android top-level entry.**
  - (a) A row in the chat list's main menu next to Saved Messages, inside `PurpleDialogsHooks`.
  - (b) A row on the Profile tab.
  - (c) A row in the main Settings list only.
  - **Default (a)**: it is where the drawer's items went, and it costs no upstream line.
- **P26 Keep downloaded scope.**
  - (a) One option for all playlists, on.
  - (b) A switch per playlist. More control, more UI, and a per-playlist field that stays local.
  - **Default (a).**
- **P27 Forced logout with unsynced edits.**
  - (a) Delete the files as upstream wipes the account. Safe for a lost device; loses up to about 10 minutes of edits.
  - (b) Keep them and offer Sync or Delete at the next login. Keeps edits; leaves playlists on a device someone else may hold.
  - **Default (a).**
- **P29 Record size caps.**
  - (a) 10 MiB compressed, 32 MiB library, 24 MiB for local edits, with in-memory downloads on both clients. Fits the existing reader and upload.
  - (b) 32, 64 and 48 MiB, which needs a file-backed download on desktop and big-file parts for records.
  - **Default (a)**: realistic libraries are about 1 MiB.
- **P30 Sync record shape in v1.**
  - (a) Full snapshots only, with the memo. Simple, proven, about 350 KB for 7,200 songs.
  - (b) Deltas with compaction triggers. Less traffic for huge libraries; much more machinery.
  - **Default (a).**
- **P31 (new) Which record machine playlists use.**
  - (a) Their own record family in the same chat, with the cover rule (this design). Works without settings sync, merges by join, 10 MiB gzip records.
  - (b) Core's reserved library stream in the settings envelope, one edit-in-place record per install. One publisher and one ledger for both families; but it needs settings sync joined first, caps records at 4 MiB of JSON, and makes settings chat moves wait for playlist stages.
  - **Default (a)**, mainly because P21 asks for playlist sync on by default.
- **P32 (new) How playlists list records in the magic group.**
  - (a) Read the history, one page of 100 in practice. Complete, no index lag.
  - (b) Search by tag, as in Saved Messages. Same cost in a near-empty chat, with lag.
  - **Default (a).** Saved Messages keeps (b) either way.
- **P33 (new) Creating the magic group for playlists.**
  - (a) Let the layer create it on the first playlist step when none exists, and disclose it in the first-run bulletin. Matches P21 (sync on by default) and the layer's own behaviour.
  - (b) Ask first: "Purple will create a private group for sync. Create · Keep on this device". Nothing changes on the account without a tap, at the cost of one more question on first use.
  - (c) Sync playlists only once settings sync has a bound chat. No surprise, but playlists then do not sync by default for people who never set up settings sync, which departs from P21.
  - **Default (a).** The owner may prefer (b), since a new group is a visible change on the account in other clients.
- **P34 (new) A switch for protected content.**
  - (a) Keep today's always-on policy. Playlists use protected songs, and sharing offers copies.
  - (b) Add the switch the owner described in round 1 (default on) to the save and copy policies. When off, playlists refuse new protected songs and sharing skips them, with no playlist code change.
  - **Default (a)** for playlists; the switch is a user-control decision, and playlists follow either.
- **P35 (new) Where private listening lands.**
  - (a) As the first commit of each client's playlist phase (B0, D0).
  - (b) On its own, now, as a small user-control change. It is two upstream lines and a settings row, and it protects chat playback today, not only playlists.
  - **Default (b)**. It has no dependency on playlists and the owner already decided it.
- **P36 (new) Post and prune, or edit in place.**
  - (a) Post a new snapshot and prune covered ones (this design). Proven by the cover rule.
  - (b) Edit this device's own latest record in place, as settings sync now does. Fewer messages and deletes; needs C8 extended and a rule for which record each device owns.
  - **Default (a)** in v1, (b) later if the owner wants fewer messages.
- **P37 (new) Encrypting playlist files on the device.**
  - (a) Plain files in the account directory, like settings. Inspectable and simple.
  - (b) Encrypt with the account key, as the deleted-message archive does. Protects playlists on a shared or stolen device; costs a key dependency and makes Start over and debugging harder.
  - **Default (a).**
- **P38 (new) Devices bound to different sync chats.**
  - (a) Each device syncs where its layer binds it; the sync line names the chat. Simple; two devices bound differently silently stop sharing playlists.
  - (b) Also warn when a record from this account appears in the other place, for example when Saved Messages holds playlist records while the device uses the magic group. One extra search per day; catches the mismatch.
  - **Default (a)**, with (b) as a follow-up if the owner binds devices by hand.

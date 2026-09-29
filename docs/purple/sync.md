# Syncing settings between devices

`settings.toml` is per-install. Two Purple Telegram clients on two machines
have two files, and nothing in the fork keeps them in step. This document is
about the one mechanism that does, and about the ones deliberately not built.

For a proposed account-backed sync flow for settings and future playlists, see
[account_sync_plan.md](account_sync_plan.md). The plan has not been implemented.

Two actions, and that is the whole feature:

- **Settings > Advanced > Purple > Send settings to Saved Messages** uploads
  the current `settings.toml` to your own Saved Messages as a document, named
  `settings.toml`, captioned with the schema version, the local time and the
  platform it came from.
- **Right-click that message > Import Purple settings** parses it, shows what
  it is about to do, and replaces the local file.

See [config.md](config.md) for the user-facing half.

## Why Saved Messages

The transport already exists, and so does the history.

Saved Messages is a chat that every account has, that syncs to every device the
account is signed in to, that survives a reinstall, and that keeps every version
of a file you ever put in it with a date attached. That is a sync channel and a
version history, and the fork writes neither of them. The whole of the export
side is "upload this file to this peer", which tdesktop's own send path already
does; the whole of the import side is "validate these bytes and write them to
disk", which `purple_config.cpp` already does for its own writes.

It is also the answer to trust. `settings.toml` holds chat ids and the display
names cached beside them - who you have decided is worth interrupting you, and
who is filed under a list called "noise". That is the kind of file you do not
want on infrastructure you have to reason about. In Saved Messages it sits
under the same account as the messages it describes, using Telegram's cloud-chat
encryption. Saved Messages is not end-to-end encrypted: Telegram stores cloud
chats on its servers, and other sessions signed in to the account can read the
file. The fork has added no separate storage service.

The cost is that importing is manual. You press a button on one machine and
confirm the import on the other. The desktop and Android clients notice a newer
file once at launch, but neither writes anything until you choose Import.

## What it does not do

It does not merge. Import replaces the file wholesale. Desktop keeps the
previous file as `settings.toml.bak` beside it. Android source `6df36b1d`
keeps that rolling whole-file-write backup and also writes
`settings.toml.import.bak` before an import, so a later editor save does not
erase the pre-import copy. Each backup has one slot. If a required backup
cannot be refreshed, the import aborts and leaves the current file unchanged.
There is no three-way merge and no attempt at one, because a merge needs a
common ancestor and there is nowhere here that would hold one.

It does not import on its own. The *sending* half can now be automatic -
`[sync] send_after_save_p`, off unless you turn it on, posts the file whenever
the app itself writes it, five seconds after the last write so a run of taps is
one document - but nothing about the receiving half changed. On Android the fork
notices that the other machine has a newer file and says so once, see phase 2
below, and the write still waits for a press. Two fingerprints in `state.toml`,
of the last file this machine queued to send and the last it wrote from an
import, suppress repeated posts of those bytes; an import never sends. The
queued fingerprint is currently written before Telegram confirms delivery, so
an upload failure can suppress a retry. The account-backed sync plan includes
a confirmed-send fix.
The rule is `ShouldAutoSend()` in the core, and
[work_mode.md](work_mode.md) has the reasoning.

It does not touch `state.toml` -
the active preset, the peek timer and the `... until` overrides are about what
*this* machine is doing right now, and carrying them across would be a much
larger claim than "these are my settings".

## Phase 2: offering an import on launch

Implemented on Android and desktop. Once a primary chat list has a usable UI
host, the client searches Saved Messages for `settings.toml` documents by name,
then once with an empty document search if needed. If the newest qualifying file
is newer than the local file it offers “A newer Work Mode settings file is in
Saved Messages” with an Import button. Nothing is written unless that button is
pressed, and pressing it uses the same import, confirmation, and download flow
as the message menu action.

Desktop accepts a settings document up to 4 MiB and scans up to 100 recent
documents. Android source `a950e13c` uses the same size and search limits;
the earlier Android build accepted only 64 KiB and scanned 20 documents.

The thing that was actually hard here was never the code. It was the worry that
stated the hold: an offer that appears on every launch of a machine you never
sync is worse than no offer. The rule that answers it is **one offer per
message, ever**. Each account remembers the id of the newest `settings.toml`
message it has already had an opinion about, and a message only earns an offer
if its id is higher than that *and* its date is later than the local file's. The
id is written before the line is drawn, so dismissing it, letting it time out,
and crashing halfway through all record the same thing. A machine you never sync
therefore sees each file you post exactly once and then never again, which is
the behaviour a notification should have: it tells you something happened, and
it does not keep telling you.

Two smaller decisions fall out of that rule. A message *older* than the local
file advances the remembered id too, without ever being shown - it is not worth
offering now and will not become worth offering later, and leaving it behind
would mean re-deciding it on every launch. And the freshness of the local copy
is read from the file's own mtime rather than from a recorded message date,
because an import writes the bytes and nothing else; since the write necessarily
happens after the message was sent, the mtime is always the later of the two and
the comparison stays honest.

The Android and desktop implementations share the same suppression rule and
wording. Desktop verification remains account-backed: exercising the search,
watermark, and confirmation flow requires a signed-in account with Saved
Messages, so local checks cannot verify the end-to-end offer alone.

## Rejected: git, driven from Termux

The first idea. Keep `~/.purple-telegram` in a git repository, and
have the Android side commit and push it from Termux, with the desktop pulling
on launch.

It has real merge power, and that is the only thing it has. Against it: the
intent plumbing between the Android app and Termux is fragile - it depends on
`RUN_COMMAND` permission, on Termux being installed from the right store, on a
foreground service surviving Android's battery management, and it breaks
silently rather than loudly. It needs a private remote, because `settings.toml`
holds chat ids and display names, which means the user must own and configure a
git host before the feature works at all. On desktop it needs either a git
binary invocation with credentials the app does not have, or a second automation
layer to match the Termux one. And the merge power it buys is unused: this is a
file one person edits on one machine at a time, so the conflicts it would
resolve are conflicts that mostly do not happen, and the ones that do are
resolved better by "take the newer one and keep a `.bak`".

The whole of that is infrastructure the user has to install, configure and
debug, in exchange for a merge algorithm the workload does not need.

## Rejected: a sync server

Run something small - a file endpoint with an account behind it - and have every
client push and pull.

Everything it would provide, Saved Messages already provides: an authenticated
per-user store, reachable from every device, with history and timestamps.
Building it would mean writing an auth story, hosting it, keeping it up, and
adding a second place where the file with everyone's chat ids in it lives. The
one thing it adds over the current design is that a client could poll it without
a button press - and that is phase 2 above, which gets the same result out of
the store that already exists.

Worth revisiting only if the manual button proves annoying *and* phase 2 turns
out not to fix it, which would mean the real requirement was continuous sync
rather than convenient sync. Nothing so far suggests it is.

## Implementation

    Telegram/SourceFiles/purple/purple_sync.{h,cpp}

Export reads `settings.toml`, parses it to find the schema version for the
caption, and refuses outright if it is not valid TOML - the caption would
otherwise have to claim a version the file does not have, and the machine
importing it would find out only after replacing its own. The message is built
as a `Ui::PreparedFile` from the bytes rather than from the path, with
`displayName` forced to `settings.toml` and the type forced to
`SendMediaType::File`, and handed to `session->api().sendFiles()` with an
`Api::SendAction` on the self history. That is the same call the send-files box
makes, so nothing about uploading is new code.

Import takes the bytes from the document's media view when they are already
there, falling back to the local file when the document has one. When it has
neither, it calls `document->save()` with an empty target - which loads a small
file into memory rather than onto disk - and waits on
`session->downloaderTaskFinished()`. The waiting subscription owns both itself
and the media view, and drops both once it has the bytes or the load has
stopped without them: the loader hands what it fetched to whichever media view
is active at the moment it finishes, so a view nobody is holding means bytes
nobody gets. The menu entry is left off entirely for a document larger than
four megabytes, since that is not a settings file and the in-memory path could
not hold it anyway.

It parses with `Purple::ParseSettings` before it writes anything, so a file
that is not valid TOML never reaches the disk. The write goes through the same
`QSaveFile` helper `purple_config.cpp` uses for its own writes, exposed as
`Purple::WriteConfigFile`, and the directory watcher picks the change up and
hot-reloads it - the import path deliberately knows nothing about applying
settings, only about producing a valid file.

On Android the launch-time offer lives in
`TMessagesProj/src/main/java/org/telegram/messenger/purple/PurpleSyncOffer.java`
and starts from `DialogsActivity.onFragmentCreate`. On desktop it lives in
`Telegram/SourceFiles/purple/purple_sync.cpp` and starts only when the primary
chat list has a usable UI host. Both search the self peer with a document
filter, choose the newest result actually named `settings.toml` and small enough
to be one, and compare its date with the local file mtime. The remembered id is
per account: Android uses `purple_sync_offered_id`; desktop appends it to
`Main::SessionSettings`. A message that has already been considered cannot
offer again, and an older local file advances the remembered id silently.
Everything that can go wrong on the way - no network, a search that answers with
an error, a download that never lands - is a line in the log and nothing on the
screen, because the user did not ask for any of this and the manual import is
still sitting in the message's own menu. The desktop flow still needs a
signed-in Saved Messages account for end-to-end verification.

The one place it guesses is the search query. It asks for `settings.toml` by
name, which relies on the server indexing document filenames the way the
shared-files search box does, and if that comes back with nothing at all it asks
once more with no query and sorts the newest hundred documents out itself. The
second request costs one small round trip on a machine that has never used the
feature, which is the price of the feature never silently failing to exist.

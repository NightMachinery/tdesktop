# Syncing settings between devices

`settings.toml` is per-install. Two Purple Telegram clients on two machines
have two files, and nothing in the fork keeps them in step. This document is
about the one mechanism that does, and about the ones deliberately not built.

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
exactly where the messages it describes already sit, under the same account and
the same encryption, and the fork has added no new place for it to leak from.

The cost is that it is manual. You press a button on one machine and pick a
menu item on the other, and nothing happens on its own. That is not an
oversight - see the next section - but it is the honest weakness, and if it
turns out to be annoying in daily use, the fix is phase 2 rather than a server.

## What it does not do

It does not merge. Import replaces the file wholesale; the previous one is kept
as `settings.toml.bak` beside it, a single file that is overwritten each time.
There is no three-way merge and no attempt at one, because a merge needs a
common ancestor and there is nowhere here that would hold one.

It does not run automatically, and it does not notice that the other machine
has a newer file. It does not touch `state.toml` - the active preset, the peek
timer and the `... until` overrides are about what *this* machine is doing right
now, and carrying them across would be a much larger claim than "these are my
settings".

## Phase 2: offering an import on launch

The obvious next step, and deliberately not in the first version: on startup,
look at the most recent `settings.toml` in Saved Messages, and if it is newer
than the local file, offer to import it - one line, dismissible, no automatic
write.

That is a small amount of code on top of what exists (the import path is
already written; what is missing is a search of one chat for one filename), and
it turns the feature from "sync when you remember" into "sync when you sit
down". It is held back only because it needs the manual path to be proven first:
an offer that appears on every launch of a machine you never sync is worse than
no offer, and the right rule for suppressing it is easier to write once there is
a month of real use to look at.

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

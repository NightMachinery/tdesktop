# Syncing settings between devices

`settings.toml` is per-install. Two Purple Telegram clients on two machines
have two files, and nothing in the fork keeps them in step automatically.
This document describes the manual transfer mechanisms and the alternatives
deliberately not built.

For the account-backed sync flow for settings and future playlists, see
[account_sync_plan.md](account_sync_plan.md). Desktop's **Sync across devices**
box runs that flow by hand for one selected account. **Check Saved Messages**
reads every sync record in that account's Saved Messages, reviews them against
the local file and this device's sync state, and shows one status line with at
most one next step:

- **Publish settings** when the sync space has no settings records yet. It
  checks again, confirms with the cloud disclosure, and sends this device's
  settings as the first record.
- **Join sync** when this device is not linked yet and another device already
  has exactly this file. The confirmation shows the cloud disclosure even
  though nothing is sent; joining records the other devices' versions locally.
- **Review update** when another device's newer version descends from the one
  this device last matched. A review box names the source device and time,
  lists up to six changed settings (then "and N more"; line counts when one
  side is not valid TOML), warns when the record comes from a newer schema,
  and has a **Show lines** monospace diff capped at 400 lines. **Apply**
  writes the file; **Not now** leaves everything alone.
- **Choose settings** when versions are unrelated or conflict. The same review
  box offers each other device's newest version and, when it can be sent,
  this device's own. **Use this version** only writes locally; **Use and
  share** also posts, after a fresh check, with the exact fingerprint and
  parents the choice produced. An unlinked device sees **Join with this
  version** or **Join and share** and always the disclosure.
- **Publish changes** when this device's file changed since it last matched,
  or its current version has not been posted. It checks again and confirms.
- **Finish sending** when an earlier post may not have finished and the
  check is otherwise clean. It checks again and lets the publisher either
  confirm the record already in Saved Messages or send the staged record
  once. While an earlier copy is still being sent, or failed, in Saved
  Messages, it sends nothing and asks you to wait for that copy and check
  again, or to delete the failed copy first. When Saved Messages also holds
  records this device cannot safely use, the box only explains that the post
  is paused.

**Up to date** reports how many other devices sync and when the check ran.
Invalid records, clones, another account's state, store errors and incomplete
scans are plain text with no action, and so is a local file that is missing,
not valid TOML or too large to send. While `settings.toml` is missing or does
not load and the app runs from its last working copy, `settings.toml.good`,
the check says so and offers no choice, join, update or new post until the
file is fixed or restored, because writing a synced version then would replace
the only copy of the settings the device is running. The check judges this
from the file it has just read as well as from the app's state, so a check in
the moment between a bad save and the app's reload refuses too. A post this
device staged before the fallback is the one exception: sending it writes
nothing to `settings.toml`, so the check still offers Finish sending for it,
exactly when a working file would, and says that the rest of sync waits for
the file.
Restore and Undo stay available; once a restored file loads, it replaces
`settings.toml.good` as usual. When this device is already linked and another
device has exactly its file, the check adopts that silently and reviews again.
Devices are named by the record's platform and the first four characters of
the install ID after the `in-` prefix, for example `Android 9c1d`. Desktop
writes its platform as Windows, macOS or Linux, as the manual Send action
does.

Sync records stay out of the download folder unless you put them there.
Telegram never downloads them automatically and writes no copy of the ones this
device sends. Clicking or saving a record works as for any other file: it goes
to the download folder, or Telegram asks where to save it when "Ask download
path for each file" is on. Records that earlier builds saved there stay until
you delete them.

The publisher enforces two modes. Finish sending starts it pending-only: it
confirms or posts the record already staged and refuses when nothing is
staged, so a second window that finished the post first cannot turn the
confirmation into a post of later edits. Every other action passes a request
with both the reviewed fingerprint and the parents that a fresh plan expects;
the publisher refuses a request missing either, and refuses any request when a
record is already staged. A record staged but never recorded as staged,
because Telegram quit or could not save its sync state in between, was never
sent; the next check drops it and carries on instead of reporting a store
error. There is no standing resume action any more. While a
check or post runs, the action, Undo and History restore are disabled, because
the publisher and the check hold the local sync store; a result from an
account or session that is no longer selected is dropped. Join, Apply and a
choice act on the Saved Messages snapshot of the check they came from and
re-check only the local file and sync state before writing, while every post
scans Saved Messages again first.
During a post, Close is disabled along with Check. During an inventory check,
Cancel check and Close remain available. Dismissing the layer through another
route still cancels its outstanding work.

Applying, choosing, restoring and undoing only change the local file. Each one
first saves the current file to a settings History, then writes the new
version, reads it back, and records it in the local sync store, in that order.
History keeps up to 30 exact copies of `settings.toml`, each with its reason,
time, fingerprint and a label such as "Before update from Android 9c1d", in the
owner-only `sync/history/` directory under the Purple config directory. After
an apply that wrote the file, a toast says "Settings updated from <device>."
and **Undo last update** puts the previous file back. Undo is offered even when
the file was written but the sync state could not be saved, and is not offered
when there was no file before or the old file is not valid UTF-8 text, since
those copies cannot be written back. A failed Undo keeps the row for another
try unless the copy is gone or cannot be restored. The row belongs to the
running app, not to one box: closing and reopening Sync across devices keeps
it. The next apply that writes the file replaces it, and an Undo, a History
restore or a restart removes it (History still has the copy then). Android's
row lasts while its Sync across devices screen stays open. **History** lists
entries as "<date time> · <label>"; each opens a preview with **Restore**.
The list is in the order the copies were saved, newest first, and the 30 kept
are the ones
saved last, even when the computer's clock was changed in between; each shows
the clock's time when it was saved, so after a clock change the times can look
out of order. Entries that
record a missing file, or hold bytes that are not valid UTF-8, cannot be
restored. A restore never lets History pruning remove the entry it is
restoring. A restored or undone file stays local until **Publish changes**
sends it. The status says so only when the file now differs from the synced
version, so a restore back to the synced version just says it was restored
beside "Up to date". If joining succeeds but a later step fails, the status says "Joined
sync, but ..." and that nothing was written to `settings.toml`; the next check
sees a linked device and offers the choice again.

Still not built: background checks, automatic publish after edits, editing a
setting in place from the review, retirement of superseded records, the
Android UI, and live verification on real accounts. The manual import below
keeps no History entry; it keeps `settings.toml.import.bak` and
`settings.toml.bak` instead.

The current manual transfer has two actions:

- **Settings > Advanced > Purple > Send settings to Saved Messages** uploads
  the current `settings.toml` to your own Saved Messages as a document, named
  `settings.toml`, captioned with the schema version, the local time and the
  platform it came from.
- **Right-click that message > Import Purple settings** parses it, shows what
  it is about to do, including the account whose Saved Messages hold it, and
  replaces the local file.

Desktop offers the action from main Saved Messages history, secondary
history lists and the self **Files** section. A mouse menu requires the
current document card handler or a real message-body or document-caption
glyph. Plain body and caption inline links preserve their normal behavior;
rich body text also needs the final native text cursor. A grouped hit belongs
to that exact member's attachment. Metadata, dates, whitespace, spoilers,
right actions, transcription controls, reactions, reply controls, and nested
web/log-original/fact-check previews never supply a target. Files accepts
only its own filename, icon, thumbnail and document download regions, never
the date link.

The file must be the message's own file-classified attachment in the asking
account's Saved Messages, named `settings.toml` case-insensitively and sized
from one byte through 4 MiB. Sending, uploading and currently downloading
files omit import. All routes omit it for nonempty text or message selection,
drag/select/reorder work, selection animation, active editor overlays or
unsettled or covered receiving geometry. Empty finished selection anchors
and completed touch bookkeeping do not independently exclude it. History
keyboard menus use the actual visible accessibility-focused message and
its own attachment, normally the group leader's. They never borrow a hovered
file or fall back to another member. Files keyboard and other non-mouse
menus omit import; Downloads, global media, Stories, saved music and other
peers do too.

On macOS, **Control+Return** opens the context menu for the currently
accessibility-focused message in main or secondary history. This uses the
physical Control key, not Command, and works without the macOS 15 context-menu
shortcut. The history receiver must have keyboard focus and the message must
still have a current visible view. Held-key repeats do not reopen the menu or
activate an item through this shortcut. Ordinary Return activation inside the
menu stays unchanged. The composer keeps its normal Return and Command+Return
behavior. This shortcut does not add keyboard import to Files.
Keyboard-origin menus use the focused message independently of Import
eligibility, including when selection omits Import. Hovered phone, reaction
or share handlers do not replace that keyboard target.

For keyboard menus, selected-text actions belong only to the focused message's
text. Selected-item actions use the existing selected set when the focused
message belongs to that set. Ordinary album actions keep whole-album behavior,
anchored to the focused message; the pointer does not choose an album part.
Sponsored and reaction targets also come from the focused message. Mouse and
touch menus retain their pointer-based selection and album-part behavior.

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

It does not merge. Import replaces the file wholesale. Before it does, both
clients keep the exact bytes of the current file as `settings.toml.import.bak`,
then as `settings.toml.bak`, and only then write the new file. If either
backup cannot be written, the import stops and `settings.toml` is unchanged;
when the import backup is the one that failed, `settings.toml.bak` is
unchanged too. Each backup has one slot, and the next import replaces both.
On Android `settings.toml.bak` is also refreshed by every other whole-file
write, including an editor save, which leaves the import backup alone, so the
pre-import copy survives later edits. Desktop has no in-app editor, so its two
backups hold the same bytes until the next import. Desktop writes both through
a temporary file and a rename, so their modification time is the time of the
import. Before 2026-10-01 desktop kept only `settings.toml.bak`, copied with
`QFile::copy`, which on macOS keeps the source file's modification time, so
that backup showed the date of the last edit rather than of the import.
There is no three-way merge and no attempt at one, because a merge needs a
common ancestor and there is nowhere here that would hold one.

It does not import on its own. The *sending* half can now be automatic -
`[sync] send_after_save_p`, off unless you turn it on, posts the file whenever
the app itself writes it, five seconds after the last write so a run of taps is
one document - but nothing about the receiving half changed. Since 2026-10-02
desktop posts only if a window showing the first account in the account
switcher's list is in front (the app's last active window) when the
five-second wait ends, into that account's Saved Messages; otherwise it skips
that save and logs that the account in front is not the first account, with
no ids or names. The window the save was made in does not matter. Before, it
posted into whichever account the focused window showed, so a test account in
front would have received the real settings, the mirror of the import
incident below. The pure part of the rule is
`JudgeAutoSendTarget()` in `purple_sync.h`, tested by
`purple/test_sync_import.sh`. The manual send is unchanged. On Android the fork
notices that the other machine has a newer file and says so once, see phase 2
below, and the write still waits for a press. Two fingerprints in `state.toml`,
of the last file this machine sent and the last it wrote from an import,
suppress repeated posts of those bytes; an import never sends, and neither
does a settings sync restore or Undo. On both clients,
the sent fingerprint is recorded only after Telegram confirms the post. While
an automatic post is in flight, its fingerprint is held in memory to keep
another local write from queuing the same bytes. Preparation, upload, and
request failures leave the sent fingerprint unchanged. If the local file has
changed while a post is in flight, confirmation of the older bytes does not
replace the current file's sent fingerprint. A lost server response remains
ambiguous: a later save or manual send can post the same bytes again if
Telegram accepted the first post.
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

The offer only ever looks at the active account, the one whose chats are in
front. On desktop that is the account the account switcher has selected; a
search starts only for it, and an answer that arrives after a switch to
another account is dropped without being remembered as offered, so that
account is searched again the next time its chat list is shown while it is
active. A file in the Saved Messages of another signed-in account, a test
account for example, is never offered. The offer names the account by its
name and public username, never its phone number, as “Account Name
(@username), sent 2026-09-30 21:14”, and the import question names it too:
“Import the settings sent ... to Saved Messages of ...?”. This rule came
from a desktop install that had a disposable test account signed in as a
second account: the test account's Saved Messages held a 4 MiB test
`settings.toml`, the offer did not say whose file it was, and Import replaced
the real settings.

What happens after Import is pressed differs. On Android, if the account is
no longer the active one when Import is pressed, when the question would
appear after the download, or when the question is confirmed, nothing is
imported and the app says why, and this applies to the message menu's import
as well. Desktop ties the import to the window that asked instead of to the
active account. Every desktop account window shows one account. Picking an
account in a switcher brings its window forward if it already has one,
without changing the active account. The active account changes when you
pick an account that has no window (the window you picked in switches to
it), when Ctrl-click (Cmd-click on macOS) opens a new account window, when
you pick an account in the menu-bar icon's account list, or when the active
account's window is closed. So a separate account window can show an account
that is not the active one. Since 2026-10-02 such a window may import from
its own account's Saved Messages through the message menu, and Import on an
offer already shown works the same way. The question names that window's
account, and the import keeps `settings.toml.import.bak` and
`settings.toml.bak` exactly as any other import does. The refusal is kept
for the one case where it is still right: the window that asked was closed
or switched to another account before the question appeared (for example
while the file was downloading) or before it was confirmed. Then nothing is
imported, the log says “settings import dropped: the window that asked was
closed or shows another account now”, and the active window says “Nothing
was imported, because the window that asked was closed or now shows another
account. The settings file is in the Saved Messages of Account Name
(@username): import it from a window that shows that account.” The rule is
"the asking window's session controller still exists": tdesktop destroys a
window's session controller when the window closes or changes account, and
closes its open boxes.

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
it does not keep telling you. On both clients, a confirmed post from this device
advances the id to its server message id, so the launch search cannot offer
the file this device just sent.

Two smaller decisions fall out of that rule. A message *older* than the local
file advances the remembered id too, without ever being shown - it is not worth
offering now and will not become worth offering later, and leaving it behind
would mean re-deciding it on every launch. And the freshness of the local copy
is read from the file's own mtime rather than from a recorded message date,
because an import writes the bytes and nothing else; since the write necessarily
happens after the message was sent, the mtime is always the later of the two and
the comparison stays honest.

The Android and desktop implementations share the same suppression rule,
account rule and wording. Desktop verification remains account-backed:
exercising the search, watermark, account and confirmation flow requires
signed-in accounts with Saved Messages, so local checks cannot verify the
end-to-end offer alone.

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
makes, so nothing about uploading is new code. Unlike the send-files box, it
leaves no copy of the bytes in the download folder: `Uploader::upload` skips
that copy for Purple's own posts (see
[account_sync_plan.md](account_sync_plan.md)).

Desktop captures a Purple-owned `ImportSettingsHit` on the menu invocation's
stack. The existing mouse update overloads optionally return it, reset it
before native guards, and capture once immediately before `setActive` from
the final native result and raw receiving geometry. Ordinary hover updates
request no record. There is no second hit-test traversal or acquisition from
cached drag state, hovered items, global click handlers or group fallbacks.
A keyboard record is prepared once before specialized dispatch; only a
qualified focused record clears that invocation's local dispatch link.
Later consumption can invalidate this record, never acquire a replacement.

The native result carries generic `None`, `MessageBody`, `DocumentCard` or
`DocumentCaption` provenance and an independent producer message id. Glyph
evidence comes from the underlying text result; rich text exposes one
default-false observation set only at its text-segment hit. A group rewrites
the result's member id without changing its producer id or selection offsets.
Purple requires both ids to agree, verifies current container membership,
and resolves the exact canonical message and its own document in the asking
session. Replacement controls clear provenance even if their handler is
unchanged. Native view observers expose only existing selection, interaction
and geometry state. The removal observer reads animation/shown state,
collapse gaps and unconsumed removal height; it probes no capability and
forces no layout. These small native extensions keep qualification and
actions in `purple_sync` without copying native hit testing. See
[upstream_hooks.md](upstream_hooks.md) for every extension site.

Menu actions retain weak asking-window and session context, message id,
message/document identity and date, with the window-bound Show. They retain
no view, layout or transient Info controller. Canonical identity is checked
before document access when the action runs, after download completion and
before confirmation writes. An in-progress import may finish its own
download, but cannot become an import of a different message or attachment.
The waiting subscription is destroyed when native session data is about to
be cleared, before documents are destroyed. This uses the existing download
and confirmation pipeline.

Import takes the bytes from the document's media view when they are already
there, falling back to the local file when the document has one. When it has
neither, it calls `document->save()` with an empty target - which loads a small
file into memory rather than onto disk - and waits on
`session->downloaderTaskFinished()`. The waiting subscription owns both itself
and the media view, and drops both once it has the bytes or the load has
stopped without them: the loader hands what it fetched to whichever media view
is active at the moment it finishes, so a view nobody is holding means bytes
nobody gets. It also drops the subscription and media view before session
data is cleared. The action is omitted for empty files and files larger than
four megabytes, because the in-memory path is capped there. Telegram's own
automatic download can still save an ordinary `settings.toml` to the download
folder when Saved Messages is on screen and file auto-download is enabled.
If it takes over the same document's import load, the import reads that saved
file through the existing local-file fallback. Sync records are never
downloaded automatically; see [account_sync_plan.md](account_sync_plan.md).

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
chat list has a usable UI host and its session belongs to
`Core::App().domain().active()`. The rules themselves, `JudgeImportOffer`,
`ImportAccountLabel` and `WriteImportedSettings`, are inline in
`purple_sync.h`, so `purple/test_sync_import.sh` can test them without the
app. Both search the self peer with a document filter, choose the newest
result actually named `settings.toml` and small enough to be one, and compare
its date with the local file mtime. The remembered id is
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

## Optional inert paint observation

The [inert attachment witness](ui_witness.md) can observe the actual normal
body draw and a qualified ordinary Import menu without invoking import. Its
whole-text markers still require the actual own Saved Messages attachment.
It is disabled by default, refuses grouped and unsupported sources, and does
not authorize file publication, input or runtime capture. Normal ungrouped
settings text uses the Message body path; ungrouped Document caption coverage
is not established.

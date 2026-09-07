# Not yet driven on a screen

Things that are written and compile, but have not been driven on a device.
Nothing here is claimed as working.

Most of this file used to be waiting on a renderer. That wait is over - the
laptop runs the APK natively on the M2's own GPU (see "Run the emulator on the
laptop, not the box" in `remote-build-and-test/readme.md`) - and two runs on it,
on the nights of 2026-09-07 and 2026-09-08, cleared the whole backlog except the
two things the emulator cannot reach at all.

Each run found one real bug, both now fixed:

- the preview menu's "Work Mode lists" entry opened nothing, because it posted
  the dialog with a zero-delay `runOnUIThread` - the next loop iteration rather
  than after the preview's dismissal animation, so the box was attached to a
  window on its way out. It now waits, and opens;
- an extra view's pinned order was lost on every cold start. `fillViewPins()`
  resolves each bare id in the file against the in-memory dialogs to recover the
  sign the file strips, and the gate loads long before the dialog list is read,
  so every pin was skipped and nothing put it back. `sortDialogs()` now retries
  the outstanding ones, which is both the moment the peers arrive and ahead of
  the sort that reads them.

For the record, those runs put up chat previews, long-press popups, overflow
menus, alert dialogs and the icon picker with **no ANR, no crash and no
renderer segfault**. Every `EXIT=139` in this project's history belongs to the
build box's software rasteriser, not to the app.

## Two the emulator cannot reach

Neither is a fork problem, and neither should be attempted again on this setup.

**The translate bar.** The decision is confirmed both ways in the log
(`translate for …: available (local premium). chat translate on.`), and the
three French messages are sitting in the bot chat for it, but the bar never
appears - because Telegram raises it from a *detected* language and detection
is Google ML Kit, whose dynamic modules a `google_apis` image cannot fetch:

    MlKitModuleManager: Modules download failed. Error code: 8
    ZappDownloader: No successful Zapp module downloads for requested modules

A Play Store image or a real device would settle it. Nothing else will.

**`VideoAds.load()`**, the media viewer's video ads: still no evidence of any
kind. It shares the channel path's guard, which is now confirmed on screen for
both the channel and the bot surface, but it has no log line of its own and the
media viewer never asked for one here.

## What the two runs actually showed

Kept because each line names the observable to re-check, not because anything
here is still open.

**The folder-count half of `hide_scope`.** With include-muted on, the default
scope took the held chat out of All chats (3 to 2) and removed the folder pill,
while the row kept its own badge on the folder tab; `keep_in_folder` put both
numbers back. The line `Purple: N chats under a hide until left out of the
folder counts` appears only in the scope that should drop the count.

**The mark on a row that is only there on a clock.** Green stripe on a held row
and on no other; the timer ring in the badge slot only on a row with no count;
the ring emptied anticlockwise unattended over ninety seconds and the row left
at expiry.

**One rule for a folder pill.** The folders popup on a long-press of the Chats
bottom tab shows the strip's numbers pill for pill: All chats 3, the extra view
P0 2, News nothing at all under `badge_p = false` - with a News chat genuinely
unread, so the absence discriminates - and News 1 the moment `badge_p` is
dropped from the same fixture.

**hide_everywhere_p.** Nine dialogs to one; gone from local search, from Recent
and from the forward picker; normal brought them all back. Pins survived the
round trip, including one made *while* everything was hidden, which is the case
the reorder guard exists for.

**The launch-time import offer.** Offer, bulletin, Import, confirmation, the
import itself and `settings.toml.bak`; a second launch suppressed by the
remembered message id alone, with the file still backdated.

**The reorder guard during a peek.** Three ways, all on screen: no **Reorder**
in the folder tab's long-press menu under a preset that restricts the strip;
Reorder back during a peek, when the strip is the account's own order again;
and still no Reorder during a peek when the preset carries an extra view,
because a strip index is then no longer a server-side position. That last one
is what `foldersRestricted()` checks before it looks at the peek at all.

**An extra view's pinned order.** After the fix above, a cold start draws the
view's pinned chat first, ahead of a chat pinned in the main list, and the log
says `Purple: extra view pins resolved.` a few hundred milliseconds after the
strip is built.

**Sponsored.** A channel and a bot chat, both directions. With Local Premium on
the log says `sponsored for …: not requested (local premium).` and no request
leaves the client; with `[premium] enabled_p = false` the same two chats log
`requested.` and `TL_messages_getSponsoredMessages` appears in the network log
(the server answered `sponsoredMessagesEmpty` both times, which is why the
request itself, not the empty surface, is the evidence).

**A notification's title.** With the bot's message let through, the shade
carries a notification titled with the *chat's* name and the message text, and
`NotificationsController` logs `value is true`. Under a preset that does not
name the bot, the same inbound message produces no notification at all and
logs `value is false` - the first empirical proof of suppression rather than
an inspection of the code path.

One caveat on that test: this fork has no FCM, so notifications come from the
background connection, and a backgrounded emulator process is frozen hard
enough that nothing arrives until it is resumed. Send the message, then bring
the app forward; the notification is posted then, for a chat that is not on
screen, which is the same code path.

**The unread-counter colours.** Seen both ways in one sitting: grey counters
with the bell under a preset that silences, blue on the same rows with the
preset lifted.

**The verdict line's `"hidden until a message"`.** Reached the way the desktop
reaches it - a row that is on screen while `shown()` is false, which on Android
means a chat an extra view holds and the preset gates. The box reads
`In 'gate': hidden until a message`, naming the rule-based list that decided it.

## What the stress suite showed

Scripted only - files, restarts, scrolls and rotation, never `monkey`, because
a random tap in a chat list can send a message to a real contact.

100 preset switches, 60 rewrites of `settings.toml`, 20 force-stop-and-launch
cycles, 80 scroll swipes and 20 rotations, back to back: **no crash, no ANR,
no native abort**, and total PSS went *down* over the run, 145 MB to 134 MB.
Six consecutive launches under the same preset logged the identical line, to
the chat - `6 of 9 dialogs hidden, 0 unread-gated (0 showing), 6 silenced` -
so the resolution is deterministic across restarts.

The hot-reload path coalesces and survives: 20 rewrites of `settings.toml` in
six seconds produced exactly one `settings.toml changed on disk` line, and a
single later edit still fired immediately, so the `FileObserver` is not lost
to the storm.

One thing the suite was not testing, now fixed in its own comment: its preset
phase rewrites `state.toml`, which is the app's own file and which nothing
watches, so a running app ignores every one of those writes and reads only the
last at its next launch. Twenty switches, zero reload lines. `settings.toml` is
the hot-reload path.

## Driving the preview menu

Worth writing down, because two earlier attempts read as "this emulator has no
preview": the chat preview opens only when the long-press lands **inside the
avatar** (`DialogsActivity` checks `cell.isPointInsideAvatar`). A long-press
anywhere else on the row is selection mode, however long it is held.

# Not yet driven on a screen

Things that are written and compile, but have not been driven on a device.
Nothing here is claimed as working.

Most of this file used to be waiting on a renderer. That wait is over - the
laptop runs the APK natively on the M2's own GPU (see "Run the emulator on the
laptop, not the box" in `remote-build-and-test/readme.md`) - and the first run
on it cleared most of the backlog in one sitting. What is left is below, and it
is a different kind of list: one real bug, two things that need a fixture
nobody has written yet, and two that the emulator cannot reach at all.

For the record, that run put up chat previews, long-press popups, overflow
menus, alert dialogs and the icon picker with **no ANR, no crash and no
renderer segfault**. Every `EXIT=139` in this project's history belongs to the
build box's software rasteriser, not to the app.

## A bug: "Work Mode lists" in the preview menu opens nothing

**This is the one failure the run found, and it is real.** Long-press a chat's
*avatar* in the chat list (not the row - `onItemLongClick` only offers the
preview when `cell.isPointInsideAvatar(x, y)`, which is why pressing the middle
of the row gives selection mode instead). The preview and its menu draw
correctly and **"Work Mode lists" is there**, between "Mark as unread" and
"Pin". Tapping it dismisses the preview and then does nothing at all: no box,
no dialog, nothing in the log, no exception.

The same box opens correctly from the selection mode's overflow on the same
chat, seconds apart, so `PurpleListBox.show()` and `PurpleListMenu.available()`
are not the problem. What differs is only the entry point:

```java
workModeItem.setOnClickListener(e -> {
    finishPreviewFragment();
    AndroidUtilities.runOnUIThread(() ->
            PurpleListBox.show(DialogsActivity.this, currentAccount, dialogId));
});
```

The comment above it says "after the preview is gone", but a zero-delay
`runOnUIThread` is not after the dismissal - it is the next loop iteration,
while the dismissal animation is still running. It was copied from the pin
action a few lines below, and that is the mistake: pinning is a data operation
that only needs the fragment alive, whereas showing a dialog needs a window
that is not mid-teardown.

Two candidate fixes, and the second is the one to prefer: post with a delay
long enough to clear the animation, or hang the call off the dismissal's own
completion rather than guessing at a duration. Whichever is chosen, the pin
action's comment should stop being cited as the precedent, because it is not
one.

## The reorder guard during a peek

`PurpleGate.foldersRestricted()` answers false while a peek is running, so the
folder tab's long-press menu should offer **Reorder** again for as long as it
lasts, and offer none without one. The strip half is verified through the log.

Still open, and no longer for renderer reasons: it needs a `state.toml` with
`peek_active` set and a deadline in the future, pushed under a preset that
names folders, and then the tab's long-press menu read in both states. Two
attempts at the long-press on the tab itself did not raise the menu at all at
the coordinates tried, which is worth a moment's care rather than a third blind
swipe - the strip's vertical position moves with whether the search bar is
showing.

## An extra view's pinned order

A view owns its pinned order, and the order itself has not been seen: chat rows
are custom views and invisible to a `uiautomator` dump, so it has to be read off
a screenshot. What is verified is that the pins are built and break nothing.
What to check is simply that the named chat is at the top of that tab. Needs a
fixture with a `[[presets.*.views]]` carrying `pinned`.

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
kind. It shares the channel path's guard and has no log line of its own.

## Still wanted on a screen

- A sponsored post failing to appear in a channel that serves one, and the
  sponsored top bar in a bot chat. The log says
  `sponsored for …: not requested (local premium).` both ways; what has not
  been seen is the surface it governs.
- A **notification's** title, the last surface Telegram's language pack was
  shadowing. Needs an inbound message with the app in the background - and see
  "The app's own name" in `defaults.md`, because the chat list header turned
  out not to be a string at all.
- The unread-counter colours, the readable evidence of an effective mute, only
  ever checked on one screenshot.
- The list box's checkboxes: adding and removing a chat from a list through the
  box, and the chat list reflecting it immediately. The box itself is verified,
  its checkboxes are not.
- The verdict line's `"hidden until a message"` wording. The other four
  readings are confirmed on screen; this one needs a chat that is *currently
  hidden*, and a hidden chat cannot be long-pressed out of a list it is not in.
  Reaching it means search, or the folder tab, rather than the chat list.

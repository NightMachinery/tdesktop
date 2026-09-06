# Not yet driven on a screen

Things that are written and compile, but have not been driven on a device.
Nothing here is claimed as working.

Most of this file used to be waiting on a renderer. That wait is over - the
laptop runs the APK natively on the M2's own GPU (see "Run the emulator on the
laptop, not the box" in `remote-build-and-test/readme.md`) - and the first run
on it cleared most of the backlog in one sitting. It also found one real bug and fixed it in
the same night: the preview menu's "Work Mode lists" entry opened nothing,
because it posted the dialog with a zero-delay `runOnUIThread` - the next loop
iteration rather than after the preview's dismissal animation, so the box was
attached to a window on its way out. It now waits, and opens. What is left
below is two things that need a fixture nobody has written yet, and two the
emulator cannot reach at all.

For the record, that run put up chat previews, long-press popups, overflow
menus, alert dialogs and the icon picker with **no ANR, no crash and no
renderer segfault**. Every `EXIT=139` in this project's history belongs to the
build box's software rasteriser, not to the app.

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
- The verdict line's `"hidden until a message"` wording. The other four
  readings are confirmed on screen; this one needs a chat that is *currently
  hidden*, and a hidden chat cannot be long-pressed out of a list it is not in.
  Reaching it means search, or the folder tab, rather than the chat list.

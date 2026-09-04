# Waiting on a better emulator

Things that are written and compile, but have not been driven on a device.
Nothing here is claimed as working. The test emulator on the shared build box
renders in software, and some surfaces cannot be reached there at all; this file
is what to walk through once an emulator with a real GPU is available.

See `remote-build-and-test/readme.md` for the renderer's limits, and
`roadmap.md` for what is actually done.

## The Work Mode list box in the per-chat preview menu

The membership box is offered from two places. The selection mode's overflow is
verified. The **per-chat preview menu** - long-press with preview, which is the
closer analogue of the desktop's right-click - is not: reaching it needs a
preview that the software renderer does not put up, and the emulator segfaults
when it tries.

Both entry points call the same `PurpleListBox.show()` behind the same
`PurpleListMenu.available()`, so the logic underneath is the verified one and
what is untested is the entry point itself. What to check:

- The entry appears in the preview menu, and only when a list has been written.
- Choosing it dismisses the preview and then shows the box, rather than putting
  the box behind the fragment being dismissed.
- Adding and removing from there behaves as it does from the overflow, and the
  chat list reflects it immediately.

## The reorder guard during a peek

`PurpleGate.foldersRestricted()` answers false while a peek is running, so the
folder tab's long-press menu should offer **Reorder** again for as long as it
lasts. The strip half of that is verified - the log says
`folder strip showing 4 of 4 (peeking)` where the same preset says `2 of 4`
without one - but the menu entry itself has not been seen, because reaching it
means a long-press popup and that is what the software renderer dies on.

Both answers come from the same flag on the same line, so what is untested is
the menu, not the decision.

## Verifying under a real renderer

Worth re-running on a GPU-backed emulator even though they passed under
software rendering, because several were driven through the app's log rather
than the screen:

- The folder strip cases: a preset naming a subset, `"*ALL"` in place, a
  misspelled name, and the reorder guard appearing and disappearing with
  `foldersRestricted()`.
- The archive override: a folder's archived chats pulled into the view, in date
  order, with the Archive row still holding them.
- The unread-counter colours, which are the readable evidence of an effective
  mute and were only ever checked on one screenshot.

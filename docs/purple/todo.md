# Not yet driven on a screen

Things that are written and compile, but have not been driven on a device.
Nothing here is claimed as working.

Most of it has been waiting on a renderer. That wait is over: the laptop now
runs the APK natively on the M2's own GPU, so the popup-heavy items that
segfaulted the build box's software rasteriser are reachable. See
"Run the emulator on the laptop, not the box" in
`remote-build-and-test/readme.md`; `roadmap.md` has what is actually done.

The order to work through it, cheapest first and sharing setup: the list box's
new rows and the preview-menu entry, then the reorder guard and an extra view's
pinned order, then the App Icon picker, then Local Premium on the screen, then
the folder unlock - which needs its own trick, described below.

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

## The list box's new rows

The box grew a verdict line at the top and the three "Show/Hide/Notify
until..." rows at the bottom, and none of it has been on a screen: both ways in
- the selection overflow and the preview menu - are popups, and popups are what
kills the software renderer. Three attempts, three `EXIT=139`.

What is untested is the presentation, not the decision. Every rule underneath is
verified through the file and the log: a hide takes the chat out of the list and
silences it, a show reveals without un-silencing, a notify un-silences without
revealing, a peek outranks a hide, and an expiry prunes itself. What to check:

- The verdict line reads correctly for a chat in a list, for one in none, for a
  gated one ("hidden until a mention"), and for one a folder pulled in
  (", shown by a folder"). This is the only path that exercises
  `deciderNative()`.
- The three rows open the spans, and a span writes the decision.
- The Cancel row appears only while something is running, and names it.

## An extra view's pinned order, and the reorder guard it forces

A view owns its pinned order, and the order itself has not been seen: chat rows
are custom views and invisible to a dump, so there is no way to read the order
off the screen. What is verified is that the pins are built and break nothing -
the tab, its membership and its badge all behave with a `pinned` key present.
What to check is simply that the named chat is at the top of that tab.

The same run should check the reorder guard, which a view now forces on:
`foldersRestricted()` returns true whenever a preset declares one, ahead of the
peek test, so the folder tab's long-press menu must offer no **Reorder** even
mid-peek. That needs a long-press popup, which is what the software renderer
dies on.

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

## Local Premium: seen in the log, not yet on the screen

Sponsored messages and whole-chat translation are now each confirmed both ways
by lines the app writes:

    sponsored for 981122490: requested.                 (enabled_p = false)
    sponsored for 981122490: not requested (local premium).

    translate for 981122490: withheld. chat translate on.        (enabled_p = false)
    translate for 981122490: available (local premium). chat translate on.

The `chat translate on` clause matters: it rules out the master switch as the
reason, so the only thing that moved is the unlock. The translate line had to be
moved out of `isDialogTranslatable` to get the withheld half at all - upstream
skips language detection when the feature is unavailable, so a line on that
predicate can only ever fire one way.

**The folder unlock is still unexercised**, and both plans for it so far
were wrong. Worth writing down, because the second wrong one looked obvious.

The first was to write `dialogFiltersLimitDefault = 1` into `mainconfig.xml` so
the account's three folders would exceed the limit. The server sends
`dialog_filters_limit_default` in the app config and
`MessagesController.applyAppConfig` writes it straight back - it was 10 again
within seconds. **A key the server writes cannot be used as a test fixture.**

The second was to make eleven real folders through the UI. That cannot be done
at all: `FiltersSetupActivity:737` refuses to open the create screen once
`count - 1 >= dialogFiltersLimitDefault && !getUserConfig().isPremium()`, and
the server enforces the same cap behind it. A non-premium account cannot hold
eleven folders, which is the point - the locked state does not arise from
making too many, it arises from *having had* Premium and losing it, leaving
folders the limit no longer covers.

Note what that means for the feature as shipped: Local Premium removes the
padlock from folders beyond the limit, and deliberately does **not** raise the
creation limit, because the server would refuse the eleventh folder anyway.
Unlocking what exists is the whole of what the client can honestly offer.

The route that should work is to stop the app config from arriving rather than
to fight it after it does: with the radio off, `applyAppConfig` never runs, and
the value read out of `mainconfig.xml` at `MessagesController:1714` stands. Set
it to 1 with the app stopped, enable airplane mode, launch, and read

    Purple: N folders against a limit of M, K locked

both with `enabled_p = true` and with it false. Three folders against a limit of
1 should be 2 locked without the unlock and 0 with it, and the padlock should
appear and disappear on the tab strip to match.

That also gives A3's folder strip and A4's folder mechanism their first test
against a limit that bites, though not against more than three folders - which,
per the above, this account can never have.

### Still wanted on the screen, whatever the log says

A log line proves the branch was taken; it does not prove the surface it governs
changed. None of these has been seen:

- a sponsored post failing to appear in a channel that serves one, and the
  sponsored top bar in a bot chat;
- the translate bar at the top of a foreign-language chat, appearing and
  disappearing with the unlock. Three French messages are sitting in the bot
  chat for exactly this;
- a folder tab drawn without its padlock, and reachable rather than opening the
  upsell;
- **`VideoAds.load()`**, the media viewer's video ads, which shares the channel
  path's guard and has no log line of its own - the one sponsored surface with
  no evidence at all.

For next time: **a build degrades gracefully at load ~100 and a renderer does
not.** That is why the log route was taken rather than waiting for the box.

## The app's own name

Fixed and seen: the chat list header reads `Purple Telegram` in a `uiautomator`
dump where it read `Telegram` before, and the system's own ANR dialog says
"Purple Telegram isn't responding". What has not been checked is the other
surface the language pack was shadowing - a **notification's** title - which
needs an inbound message with the app in the background.

## The launcher icon, in the picker

The alias is verified as far as the package manager goes: `PurpleIcon` is
registered, the only launcher entry on a fresh install is `DefaultIcon`, and
enabling ours makes it the launcher entry and disabling it puts the stock one
back. What that does *not* cover is the two things a person would actually do:

- open **Settings > Chat Settings > App Icon** and see a Purple tile in the
  list, drawn from `icon_purple_background_sa` behind the stock foreground;
- tap it, and have `LauncherIconController.setIcon()` leave exactly one alias
  enabled - the guarantee that stops two Purple Telegram entries appearing on
  the home screen at once.

Both need the picker on screen, so both are waiting on a renderer. The artwork
itself has been looked at outside the app and is right.

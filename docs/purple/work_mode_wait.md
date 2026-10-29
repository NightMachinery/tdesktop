# Waiting before Work Mode changes

Desktop uses the existing Peek cover and `[peek] start_delay_seconds` for Peek
starts, restarts, extensions, preset switches, account Own/Follow/Stock choices,
and app-wide source choices that would enter Normal. The menu and Settings use
the same preset picker. Preset hotkeys and the Peek hotkey use the same request
path. The Peek hotkey already honored the start delay before this change.

The rule is deliberately coarse. Leaving Normal for a restrictive preset,
leaving Stock for Own or Follow, ending Peek, and no-ops are instant. Entering
Normal or Stock, switching presets, and switching Own/Follow wait. **Switching
between two restrictive presets also waits, even when the new one is stricter.**
There is no comparison of list, folder, story or notification policies.
An app-wide source choice waits when its existing review says it enters Normal.

Desktop and Android default to ten seconds when the value is absent. An explicit
value, including zero, applies to both. Increasing the delay is restrictive and
instant through the option request.
Decreasing it waits for the longer of old and new. Effective no-ops are instant
without writing, including an absent value resolving to the same ten seconds.
There is no separate Work Mode delay setting. Desktop reuses the shared
`kPeekStartDelayMobileDefaultSeconds = 10` constant. Android declares it in
`TMessagesProj/jni/purple/purple/purple_engine.h:714` and exports its resolved
value as `peekStartDelay` in `TMessagesProj/jni/purple_jni/jni_bridge.cpp:1320`.

A documented recovery timing difference remains: Desktop waits when a missing
saved preset is changed to Normal. Android applies this recovery immediately
because its effective preset is already Normal.

`RequestWait` is the one UI request path. It captures and validates the opening
account and write context before classification, explains a held write immediately,
then shows the countdown if required. No countdown starts for a refused capture.
Completion validates the original context again and calls the existing guarded
writer. A fresh settings capture never substitutes a different destination for
an old editor. Ending Peek stays available even when settings cannot be edited.

Abort, any key during the countdown, closing the owning window and switching to
another app cancel the pending operation. Locks do not: see
[The ready step](#the-ready-step). Key presses on the cover or its buttons are
handled quietly; the temporary application filter also intercepts shortcuts.
It is removed synchronously at cancellation/completion, before deferred cover
deletion. Subsequent keys work normally. A pending wait is never persisted.

## The ready step

@Opus5.5 [2026-10-28 Tue]. When the countdown ends nothing is applied. The cover
becomes a **ready step**: a heading, "Press Enter to confirm. Any other key
aborts.", the action button and Abort. The button names the action: "Start
Peek", "Extend Peek", "Switch to <preset>" or "Apply change". The change
applies only on that button, so a Peek's duration starts at the press. Every
route in this document gets the step: Peek starts and extensions, preset
hotkeys and pickers, missing-preset recovery, per-account list modes, app-wide
source choices and the Peek option editors. Zero delay, only-tightening
changes, no-ops and the sync and automation exemptions get neither countdown
nor step. Nothing announces the end of the countdown: no notification, sound or
window raise.

Pressing the button re-runs today's completion: the generation check, the
validation of account, session, preset, state and exact disk input, the
foreground and unlocked check, and the guarded writer. A failure aborts with
today's message.

Keys in the ready step: Return or keypad Enter presses the button; Escape and
any other key abort. For 500 ms after the step appears, and again when it
reappears after a lock, every key is swallowed and the button is greyed and
deaf to the mouse, so a held or late key cannot confirm. Auto-repeat is always
ignored. Abort stays clickable throughout. The ready step's Abort sits exactly
where the countdown's Abort was, with the action button, hint and title stacked
above it, so a late click aimed at Abort can never land on the action, however
narrow the window or long the text.

The step aborts by itself, quietly, `[peek] ready_timeout_seconds` after the
countdown ended: default 600, at least 10. It counts from the end of the
countdown, so a Mac that sleeps past the deadline does not wake into a fresh
step. All wait times use `WaitClockNow()`, a monotonic clock that keeps
counting in sleep (`CLOCK_MONOTONIC` on macOS, `CLOCK_BOOTTIME` on Linux,
`GetTickCount64` on Windows). `crl::now()` stops in sleep on macOS and Linux
until something adjusts it.

### Locks versus app switches

Switching to another app cancels, in both phases. Screen lock, OS session lock,
sleep and Telegram's passcode lock do not. On return the countdown or ready
step is still there, unless the timeout passed.

The signals are the ones Desktop already has. All feed
`Core::App().screenIsLocked()`:

- macOS: `com.apple.screenIsLocked` and `com.apple.screenIsUnlocked`;
- Windows: `WTS_SESSION_LOCK`/`LOGOFF` and `UNLOCK`/`LOGON`;
- Linux: the XDG portal's `screensaver-active`;
- passcode lock, on every platform: `passcodeLocked()`.

A lock can make the app inactive before its signal arrives, so `WaitPresence`
never acts on inactivity alone:

- leaving the window leaves 300 ms for a lock signal;
- a lock signal inside that makes this a lock;
- coming back without a lock, or the 300 ms passing, cancels;
- after an unlock the window enters Returning and must regain the foreground
  before the 2 s settle expires.

Returning checks expiry first: at exactly 2 s, or later, the wait cancels even
if a lock or foreground signal arrives on that tick. A relock before expiry
also cancels unless an earlier unlocked foreground update already moved the
phase to Present. Foreground and lock arriving together are not evidence of
an unlocked return. After an unlocked foreground return, leaving again uses
the ordinary 300 ms lock window: a lock before that boundary keeps the wait;
at the boundary it cancels.

The 300 ms is short on purpose: switching to another app and locking a second
later is still a switch, so the wait is gone, even if the person clicks back
into Telegram right after unlocking. (It was 2 s before PEEK-CONFIRM-2, which
let that sequence keep the wait.)

Where a lock cannot be told apart from a switch, the wait cancels:

- a lock signal more than 300 ms after the app went inactive;
- Linux without the portal;
- sleep that deactivates the app without a lock;
- macOS fast user switching without `screenIsLocked`.

While locked the cover is hidden, so it never sits over the passcode widget,
and the key filter passes every event through. Another Telegram window or
dialog taking activation cancels at once, except behind a lock.

An aborted restart or extension leaves the active Peek untouched. A completed
restart replaces its deadline then. A completed extension adds to its existing
deadline with the existing hour cap. A Peek that expires or otherwise changes
during the countdown cannot be extended by that pending request.

Work Mode waits retain the opening account when the displayed account changes.
Logout, session replacement, source/role changes, changed file identities or
inputs and changed device state invalidate the capture. Preset selection is
still device-wide; Own/Follow/Stock selection is still account-specific.
Peek and option-edit waits retain their stricter selected-account guard.

Incoming settings sync is exempt: it applies without a countdown or arrival
hold. The initiating device gates its own Work Mode UI. Per-account mode and
active preset/Peek state stay local. Manual sync Apply, Import and History
Undo/Restore are deliberate settings operations and are exempt in this batch.
Schedule and Focus transitions remain immediate because they were configured
in advance. Editing their configuration remains a settings operation. An exempt
apply cancels a pending request whose captured context changes. No automation
opens a countdown.

## Dedicated Peek option editors

`RequestPeekOptionChange` is the common entry for the in-app start-delay,
exit-hidden and local focus-end editors. It uses `RequestWait`, the same cover
and cancellation engine as Work Mode changes. Increasing a delay and enabling
exit-hidden or focus-end only tighten behavior, so they apply instantly. Any
loosening or mixed change waits; a changed delay uses `max(old,new)` so shortening
cannot buy a cheaper countdown. No-ops pass through validation and write nothing.
Abort writes neither the shared file nor the local preference. These edits do
not start, restart or extend Peek.

Shared edits carry the opening app-wide editor and exact file target. Both
instant and delayed paths revalidate the editor, selected account/session,
source and input. The local focus preference uses the same engine and opening
account/editor guards, with no writable settings-file requirement. A later
capture never redirects an older pending edit.

The scope is the dedicated editors only. Manual downloaded-settings Import,
manual Sync Apply and version/adoption choices, History Undo/Restore, incoming
sync and external editor saves remain exempt whole-file routes. They can change
app-wide `[peek]` values without an option countdown. The disk watcher does not
stage external saves for review. Own files cannot override global `[peek]`.
Source selection keeps its existing Work Mode bypass classification rather than
comparing individual Peek options. Duration, hotkey and lock values have no
other dedicated Desktop editor and remain configurable through those file routes.
No exemption writes the device-local focus preference. An exempt apply still
invalidates a pending request when its captured input changes.

`purple/check_wait_routes.py` checks a reviewed inventory of called writer
spellings and active-preset assignments across production Desktop/core sources.
It matches unqualified calls, namespace-qualified calls and every receiver
spelling, including declarations. `SetPeekStartDelay` and every `SetTable*` call
are inventoried conservatively even when their table/key is aliased. Non-Peek
schedule/device labels, screen-time, Premium, per-account row edits and core
declarations are reviewed inventory entries, not newly gated operations. The
core parser's `SetTables` type construction is a deliberate conservative match. `purple/wait_routes.json` records the
inventory. Request completion, guarded writer implementations, core resolution,
schedule/Focus, Last Seen Peek and automatic Peek termination are the reviewed
non-UI writers. Comments and string literals are excluded. This is a syntactic
regression check, not type resolution or a proof against aliases/reflection.
Review an inventory change rather than regenerating it without checking routes.

Run the headless gate and production key-filter tests with
`python3 purple/test_wait.py --build-dir <existing-Debug-tree>` and the inventory
negative tests with `python3 purple/test_wait_routes.py`. The event tests use
real Qt widgets, child-button focus and application shortcuts on the offscreen
platform, including Qt platform delivery through the host QWindow via
`qt_handleKeyEvent`. They do not qualify native account switching or the visible cover's
layout. File tests in `account_routing_file_cases.cpp` retain the original
guarded-writer cases and exercise diagnostic captures without file changes.

## Peek after application focus loss

Countdowns always abort immediately and quietly on host activation loss, application
inactivity or lock, including focus moving to Telegram's own media viewer/dialog.
Application-state and activation signals cancel first; the 250 ms tick is a quiet
backstop. No pending write can run after the abort. Other stale account/settings
errors remain explicit.

"End Peek when Telegram loses focus" is an off-by-default Desktop-local generic
Core::Settings preference. Enabling it is restrictive and instant. Disabling it is
loosening and goes through the same Work Mode wait; an aborted change keeps it on.
It is not in settings.toml, account state or sync and requires no core schema change.
The Peek settings section contains the toggle and explanation. Both directions
and no-ops delegate to `RequestPeekOptionChange`.

When enabled, an already active Peek ends after 2 seconds of application inactivity;
returning sooner keeps it. Telegram's other windows and file dialogs do not end an
active Peek while the application stays active. Passcode, screen and OS session
lock end it immediately. When disabled, focus loss leaves active Peek untouched,
and existing lock policy applies. The ordinary End Peek path still applies the
hidden-chat exit option. Android does not have this option yet.

Dedicated Peek option writes keep separate precommit and completion checks.
Precommit compares the opening values and exact input. After the synchronous
reload, completion checks the intended new shared values, the opening editor
and account authority, and the fresh exact write target. A successful write
refreshes that editor target before any mixed device-local preference write.
Later edits from the same open editor use the refreshed target. Other settings
writers retain their existing completion checks unless they supply a separate
committed-value check.

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
value, including zero, applies to both. Changing the delay itself waits for the
longer of old and new. There is no separate Work Mode delay setting. Desktop reuses the shared
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

Abort, any key, closing the owning window, backgrounding it, screen lock or app
lock cancel the pending operation. Key presses on the cover or its Abort button
cancel quietly; the temporary application filter also intercepts shortcuts.
It is removed synchronously at cancellation/completion, before deferred cover
deletion. Subsequent keys work normally. A pending countdown is never persisted.

An aborted restart or extension leaves the active Peek untouched. A completed
restart replaces its deadline then. A completed extension adds to its existing
deadline with the existing hour cap. A Peek that expires or otherwise changes
during the countdown cannot be extended by that pending request.

Work Mode waits retain the opening account when the displayed account changes.
Logout, session replacement, source/role changes, changed file identities or
inputs and changed device state invalidate the capture. Preset selection is
still device-wide; Own/Follow/Stock selection is still account-specific.
Peek and delay-edit waits retain their stricter selected-account guard.

Incoming settings sync is exempt: it applies without a countdown or arrival
hold. The initiating device gates its own Work Mode UI. Per-account mode and
active preset/Peek state stay local. Manual sync Apply, Import and History
Undo/Restore are deliberate settings operations and are exempt in this batch.
Schedule and Focus transitions remain immediate because they were configured
in advance. Editing their configuration remains a settings operation. An exempt
apply cancels a pending request whose captured context changes. No automation
opens a countdown.

`purple/check_wait_routes.py` checks a reviewed inventory of called writer
spellings and active-preset assignments across production Desktop/core sources.
It matches unqualified calls, namespace-qualified calls and every receiver
spelling, including declarations. `purple/wait_routes.json` records the
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
The Peek settings section contains the toggle and explanation.

When enabled, an already active Peek ends after 2 seconds of application inactivity;
returning sooner keeps it. Telegram's other windows and file dialogs do not end an
active Peek while the application stays active. Passcode, screen and OS session
lock end it immediately. When disabled, focus loss leaves active Peek untouched,
and existing lock policy applies. The ordinary End Peek path still applies the
hidden-chat exit option. Android does not have this option yet.

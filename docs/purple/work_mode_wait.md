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

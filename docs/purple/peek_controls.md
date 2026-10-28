# Peek controls

Work Mode's Peek controls are global, beside the duration controls. They do not
belong to the selected preset or account.

Exit hidden chats when peek ends defaults to on. When an active Peek ends,
the desktop re-evaluates each open conversation against the restored effective
preset. A visible chat stays open, including a show override or a chat reachable
through an enabled folder or extra view. Hide overrides and their scope still
apply. Normal closes nothing. Turning the option off leaves the conversation
open. The open-chat/recent grace buffer does not defeat this deliberate exit.
The transition hook covers manual stop, timed expiry, lock, preset changes and
state/config reloads which end an effective Peek.

Start delay is a non-negative whole number of seconds. With no configured value,
desktop and mobile use ten. An explicit value, including zero, is
shared by all devices. Existing duration, hotkey and lock defaults are unchanged.
The desktop starter file documents the delay without writing a value, preserving
the common ten-second fallback when that file is shared.

All desktop start routes enter one transient controller: shortcut, checkbox,
chips and dial. A positive delay shows a full app-content cover with a large
countdown and Abort. It covers the Telegram window, not the OS desktop.
When the countdown ends, a ready step waits for the "Start Peek" button or
Enter; see [the ready step](work_mode_wait.md#the-ready-step).
Abort or any key during the countdown cancels quietly, including when the Abort
button has focus.
The temporary application key filter is removed before the cover is deleted.
End Peek cancels pending work and ends active Peek immediately. No pending
activation is persisted. A restart or extension with positive delay leaves the
old Peek untouched until completion. The requested duration starts only when
the ready step's button commits the actual activation.

A stale window, account, session, preset, lock, settings reload or app termination
cancels pending Peek work. The cover also refuses completion if its originating
window is no longer active. There is one countdown timer, independent of the
existing active-Peek expiry timer and dial's input debounce.

The same cover now handles Work Mode changes, including preset hotkeys and
per-account list modes. Work Mode waits stay bound to their opening account
when the displayed account changes. Desktop waits for missing-preset to Normal recovery; Android applies that
recovery immediately because its effective preset is already Normal.
The deliberately coarse rule and the sync
and automation exemptions are described in [Work Mode waits](work_mode_wait.md).

The ready step timeout, `[peek] ready_timeout_seconds` (whole seconds, default
600, at least 10), has its own editor under the start delay. Lowering it is
instant; raising it waits and gets the ready step itself.

All four dedicated option editors use `RequestPeekOptionChange` on that same
cover engine. Increasing start delay, enabling exit-hidden and enabling the local
focus option are restrictive and instant. Decreases, disabling either option
and mixed changes wait; delay edits use the greater of old and new effective delays. Abort keeps the previous setting. Completing a delay
change writes only that setting, without starting Peek. Effective no-ops, including an absent delay resolving to the same ten seconds,
are validated immediately and write nothing. Unsupported TOML edits refuse rather than
rewriting unrelated content.

## Whole-file boundary

The option request gates only the dedicated in-app exit-hidden, start-delay and
local focus editors. Manual Import, Sync Apply/version/adoption, History
Undo/Restore, incoming sync and external editor saves remain exempt when they
replace the app-wide file. An external save is applied by the disk watcher
without staging a countdown. Source choices retain the existing Work Mode
bypass rule, not a comparison of individual Peek values. Own files cannot
override `[peek]`; the remaining duration/hotkey/lock keys have no dedicated
Desktop editor. None of these file routes contains the local focus preference.
A changed source or input invalidates a pending option request. See
[Work Mode waits](work_mode_wait.md#dedicated-peek-option-editors).

## Shared schema and mobile integration

The shared core reads `[peek] exit_hidden_chats_p` through
`PeekExitHiddenChats(settings)`, default true. It reads the optional integer
`start_delay_seconds` through `PeekStartDelaySeconds(settings, device)`. The
core still distinguishes device classes for this accessor. Desktop's UI uses
`PeekWaitDelaySeconds(settings)`, retaining explicit values and applying the same
`kPeekStartDelayMobileDefaultSeconds = 10` fallback as Android. Android declares
that constant in `TMessagesProj/jni/purple/purple/purple_engine.h:714`, resolves it
in `purple_engine.cpp` and exports it as `peekStartDelay` through
`TMessagesProj/jni/purple_jni/jni_bridge.cpp:1320`; Java's
`PurpleCore.Clock` also defaults that field to 10. Invalid/negative,
fractional, string, boolean and overflowing delays warn and retain absence.
`SetPeekStartDelay(text, path, seconds)` edits the scalar while preserving
comments, unknown keys and unrelated tables, refusing unparseable/unsupported
layouts. `SetTableBool` edits the exit option. No binary fields were inserted.

Android must bind these same accessors and schema, keep one lifecycle-owned
pending activation for all routes, and validate the current account, host,
preset and locks at completion. Start the real duration only at completion.
Apply only-tightening and no-op edits instantly through the request; other edits
wait, with delay decreases using max(old,new) and Abort retaining the old value.
The required mobile End Peek strip button appears only during active Peek and
ends it immediately. The desktop change does not implement that Android UI.

## Qualification

`Telegram/SourceFiles/purple/tests/peek_admission_cases.cpp` covers ownerless
settings loading with both immediate and delayed starts, changed input and
routing metadata, account/window/lock guards, and every Peek refusal reason.
The file fixture in `peek_file_cases.cpp` retains the exact-write controls.

Core TestPeekControls cases cover the shared schema accessor's device defaults,
explicit zero, shared values, invalid input, comment/unknown-key preservation,
idempotence and refused edits. Desktop's `purple/test_wait.py` separately covers
its ten-second fallback, no-op source confirmation, wait state machine and real
Qt platform-key delivery, including shortcuts and an already-running Peek.
These headless tests do not qualify native account switching or cover layout.
Native cases must still cover manual/expiry/lock/preset termination,
visible/hidden/override/folder/off behavior, every start route, abort/second/third
press, duration timing, stale context and both directions of delay editing.

## Upstream hooks

`Telegram/cmake/td_ui.cmake` includes the Purple-owned style list before the
normal generator. All added runtime code, style definitions and other build
registration live in Purple-owned files. The existing window hotkey hook is
reused; no new application/controller implementation was placed upstream.

## Settings freshness and deliberate exit

Starting and extending Peek read settings and change device state. They do not
require an app-wide settings edit route or a root-file owner. A readable,
accepted primary file can therefore run Peek while account ownership is unset.
The routing loader retains its selected primary or recovery source, the exact
bytes accepted for that load, its effective-settings verdict and routing context,
including ownerless mode. Peek uses the receipt associated with the settings
actually applied to the running filter. It requires an accepted primary whose
current bytes and routing metadata still match that receipt, and rejects a
symlinked primary just as the loader does. Recovery settings cannot authorize it.
Preset and file-option edits keep their existing edit guards. Work Mode off, an
ended Peek, an untimed Peek, the extension limit and an invalid extension each
have a separate refusal message.

Entry checks synchronously refresh the active state input and validate the
loader receipt before starting even a zero-delay request. Countdown completion
checks it again. A save between the outer watcher read and the loader's read
cannot substitute the outer bytes for the actual accepted input, including an
A-B-A save sequence. A mismatch refuses until the running loader has accepted
matching current input. This compares accepted content and routing context; it
does not claim to detect every transient edit restored before either check.
Missing files, read errors and readable-empty inputs are distinct.

At countdown completion, exact disk-input validation can synchronously notify
subscribers and cancel pending state. The operation generation is checked after
that validation, then local copies of the originating context and requested action
are rechecked after cancellation. No cleared pending fields are used to activate
Peek or change its delay. A loosening option change waits, with delay decreases using max(old,new); Abort
keeps the old shared and local values.

Supported settings splice writes compare their expected input before splicing and again
after writing the QSaveFile temporary, immediately before commit. A stale input
refuses without overwriting newer external bytes, including unrelated edits. This
is a precommit expected-input guard, not a universal compare-and-swap against an
external writer that ignores coordination: a noncooperating atomic save in the
last check-to-rename interval remains a limit.

Deliberate Peek exit uses the existing visibility rules with opened-only unread
and recent/open grace excluded. Actual unread messages, manual unread marks,
mentions and reactions retain their meaning. Folder predicates explicitly ignore
the opened-only flag, and genuine extra-view membership remains reachable. The
normal navigation predicates keep their original default behavior. Option-off,
Normal, explicit Show overrides and genuinely visible chats stay open.

## Application focus and locks

A pending countdown or ready step aborts quietly when another Telegram window or
dialog takes focus, or when the application goes inactive and no lock signal
follows within 300 ms. Screen, OS session and passcode locks and sleep keep it:
see [locks versus app switches](work_mode_wait.md#locks-versus-app-switches).
Application-state, activation and lock events drive it; the 250 ms tick is a
backstop. Abort leaves the requested write
unapplied and shows no context-changed toast, including reentrant focus cancellation.

Desktop's Peek section has "End Peek when Telegram loses focus", off by default.
It is stored only in this device's Core::Settings generic preference map, not in
settings.toml or account sync. Turning it on is restrictive and instant. Turning it
off is loosening and uses the ordinary Work Mode countdown; Abort keeps it on.

With the option on, active Peek ends after 2 seconds of sustained application
inactivity, and immediately on passcode, screen or OS session lock. Returning
within the grace keeps it. The active-Peek monitor follows Qt application state,
so Telegram's media viewer, another Telegram window and its native file dialogs
do not end it while Telegram remains the active application. Ending uses the
ordinary End Peek path and honors Exit hidden chats when peek ends.
With the option off, focus loss does not end active Peek, and existing per-device
lock settings keep their current behavior. Android does not have this option yet.

The grace belongs to the active Peek's existing active/deadline identity, the same
tuple the core uses to distinguish a new start from a no-op. A live state reload
that replaces its deadline drops the old grace and starts a fresh two seconds if
Telegram is still inactive. Unrelated state reloads do not extend that grace.
The writer inventory covers both literal and locally aliased writes of the local
preference. Every write stays inside RequestPeekOptionChange, including instant
enabling; a direct disable outside it fails the route guard.

The headless wait fixture covers Qt application-state delivery, a second in-app
window, both grace boundaries, ten seconds inactive with the option off, immediate
locks with it on, quiet countdown cancellation and restrictive/loosening setting
changes. Native focus/file-dialog and lock delivery remain platform qualification;
the tests do not launch Telegram.

Dedicated Peek option writes keep separate precommit and completion checks.
Precommit compares the opening values and exact input. After the synchronous
reload, completion checks the intended new shared values, the opening editor
and account authority, and the fresh exact write target. A successful write
refreshes that editor target before any mixed device-local preference write.
Later edits from the same open editor use the refreshed target. Other settings
writers retain their existing completion checks unless they supply a separate
committed-value check.

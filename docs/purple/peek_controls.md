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
Abort or any key cancels quietly, including when the Abort button has focus.
The temporary application key filter is removed before the cover is deleted.
End Peek cancels pending work and ends active Peek immediately. No pending
activation is persisted. A restart or extension with positive delay leaves the
old Peek untouched until completion. The requested duration starts only when
the countdown commits the actual activation.

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

Changing start delay uses that same cover and waits for the greater of the old
and new effective delays. Abort keeps the previous setting. Completing a delay
change writes only that setting, without starting Peek. A zero-to-zero explicit
configuration commits immediately. Unsupported TOML edits refuse rather than
rewriting unrelated content.

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
Apply delay changes after max(old,new), with abort retaining the old value.
The required mobile End Peek strip button appears only during active Peek and
ends it immediately. The desktop change does not implement that Android UI.

## Qualification

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

A Peek request captures the actual readable settings-file bytes. Entry checks
also synchronously refresh the active state input. If disk and the loaded settings
input differ, the existing parser/reload route runs and the request refuses; a
second explicit request uses the refreshed delay. This includes the zero-delay
path, so a stale cached zero cannot bypass a larger saved delay. Missing files,
read errors and readable-empty inputs are distinct. Invalid primary settings do
not authorize a countdown using the last-good fallback.

At countdown completion, exact disk-input validation can synchronously notify
subscribers and cancel pending state. The operation generation is checked after
that validation, then local copies of the originating context and requested action
are rechecked after cancellation. No cleared pending fields are used to activate
Peek or change its delay. Changing a delay still waits max(old, new); Abort keeps
the old file value.

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

A pending countdown always aborts immediately and quietly when its host window
loses activation, another Telegram window or dialog takes focus, the application
becomes inactive, or the screen/app locks. Application-state and activation events
cancel it directly; the 250 ms tick is a backstop. Abort leaves the requested write
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
preference. Every write stays inside RequestPeekFocusLossChange, including instant
enabling; a direct disable outside it fails the route guard.

The headless wait fixture covers Qt application-state delivery, a second in-app
window, both grace boundaries, ten seconds inactive with the option off, immediate
locks with it on, quiet countdown cancellation and restrictive/loosening setting
changes. Native focus/file-dialog and lock delivery remain platform qualification;
the tests do not launch Telegram.

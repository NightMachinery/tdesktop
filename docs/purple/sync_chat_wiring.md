# Sync chat changes: how the setup screen runs them

A *carry* is one journaled change of where an account keeps its sync data:
a Change sync chat move, a Follow or Fill of another device's chat, a Lost
recovery, or the removal of one older Purple mark. The journal, its phases and
the executor are described in [move storage and recovery](sync_chat_move.md).
This page covers how Desktop starts, shows and stops carries.

Each feature has its own switch, a constant that is either ON or OFF:

- Move: `DesktopSyncChatMoveActivated` in `purple_sync_chat_setup.cpp`;
- Follow/Fill: `DesktopSyncChatFollowFillActivated` in `purple_sync_chat_consent.cpp`;
- Mark removal: `DesktopSyncChatMarkRemovalActivated` in `purple_sync_chat_mark_actions.cpp`;
- Cleanup: `DesktopSyncChatCleanupActivated` in `purple_sync_chat_cleanup.cpp`;
- Recovery: `DesktopSyncChatRecoveryActivated` in `purple_sync_chat_recovery.cpp`.

The activation turns each switch ON in its own one-line change, so any one can
be held back. The first release may keep Cleanup OFF. A switch that is OFF keeps
its control's OFF result: no row, or a label saying the change is held. With all
five OFF, nothing on this page runs: the carry service is never constructed and
startup goes straight to ordinary reconciliation. With Cleanup OFF and the other
four ON, a move never offers or sends cleanup, a Follow plans none for its late
posts, and the old chat's settings messages stay where they are. What each change does for the user is in
[Syncing settings between devices](sync.md#changing-the-sync-chat).

## One owner per account

`DesktopSyncChatCarryOwner` (`purple_sync_chat_carry_owner.cpp`) runs at most one
carry for an account. It reuses the existing controllers (the consent flow, the
recovery flow and the mark actions) and the existing executor, so there is one
journal and one selection algorithm. A second request while one is running is
refused and the screen says another change is running.

`DesktopSyncChatCarryService` (`purple_sync_chat_carry_service.cpp`) is the
owner's native host. It is created on first use for a live session and destroyed
with it. It supplies:

- a dedicated store thread. The carry store binds to the thread that opened it,
  so every store task must run on the same thread; a GCD queue may switch threads;
- fresh resolution and fresh peer facts before each step;
- provision-only channel creation through the existing setup service, with Retry
  resuming the same create journal;
- the account's carry lease and publisher quiescence before a journal opens;
- the native transport and mark port from `purple_sync_chat_native.h`.

The native transport renews every effect before it sends it, the first one
included. The renewal asks the carry's own engine, on the store thread, for a
current permit. A move or a startup resume asks the owner's engine. Follow/Fill
and recovery ask their controller's engine through `withEngine`. Both paths check
on the GUI thread and again after the store-thread hop that the same carry is
still running and its account still current. A carry that has stopped gets no
permit, so the transport sends nothing.

## Startup

When discovery starts a session, `ResumeSyncChatCarry` resumes an unfinished
journal first and runs ordinary reconciliation when it settles. With Move,
Follow/Fill and Recovery OFF, it is ordinary reconciliation alone.

The resume can *wait*: it is refused at startup because the account's context
is not current yet (the app is inactive or locked, the account's sync scope is
not captured yet, or its setup context is not current). It can also *stop
early*: it starts, but ends before its engine opens the journal, because the
context changed, the carry lease is busy, or no transport is available. In both
cases reconciliation runs now, and the resume runs again on the next activation
or unlock, and on the next resolver status or settings change. Status and
settings changes start it at most three times between activations, so a stop
that persists does not loop. A resume that stopped later, with its journal open,
keeps its own Retry.

A journal that cannot be read, or belongs to another account, is not treated as
absent. The resume ends held without creating or sending anything, and discovery
keeps sync held and reports it. The resume does not retry by itself.

Every wait and stop is logged once per change, with codes and enums only:

- `sync chat carry resume waits, check <check>`: the first check that refused
  it, one of `session`, `scope`, `app-inactive`, `app-locked`, `setup-context`,
  `owner-busy` or `owner-refused`;
- `sync chat carry resume opening stopped, check <check>`: why no resumable
  journal was read, for example `no-carry-marker` (nothing to resume),
  `journal-read:<store status>`, `journal-invalid`, `state-binding`,
  `cache-paused` or `cache-login`;
- `sync chat carry resume <reason>, <running|stopped|finished>, store <status>,
  journal <phase>`: each change of the resume's reason or outcome.

## Stopping

A carry belongs to the setup window that started it, and a startup resume belongs
to no window. These stop the running carry:

- closing its window, including Cancel;
- leaving the app or locking it;
- logging out, replacing the session, or a change of the account's sync scope.

Stopping cancels the executor between requests and keeps delivered copies. A step
already in flight keeps its journal. After leaving or locking the app, that
journal resumes once the app is active again. Otherwise the setup screen offers
Resume unfinished change, and the next startup resumes it.

## The setup screen

- Change sync chat: Continue reviews the choice, then starts the move. Choosing a
  new channel creates it first (provision only), then moves into it.
  The cleanup choices, the Saved Messages cleanup warning and the confirmation's
  sentence about removal appear only for an ordinary move with Cleanup ON,
  because only that move sends them. Where no Change button is offered, the
  screen says Change appears once the account uses a confirmed sync chat and no
  other change runs (Move ON), or that changing is disabled (Move OFF).
- Lost recovery: Create, Choose and Mark this chat again start the recovery flow.
  It leaves the lost chat alone, so it offers no cleanup.
- Follow: shown when resolution asks to Follow or Fill. It reads the winner's
  readers, shows the disclosure, and offers Follow, Pause sync here or Cancel.
  The owner captures Cleanup when it admits the Follow. Only with Cleanup ON does
  a Follow out of a chat other than Saved Messages delete this device's own
  settings posts that it copied from the old chat; with Cleanup OFF it leaves
  them and finishes with no cleanup step. The Follow disclosure says those posts
  are deleted exactly when that can happen. With Follow/Fill ON, the status says
  the account has a newer sync chat and points to Follow.
- Remove older Purple mark: removes only the confirmed row.
- Old settings cleanup: shown while a planned cleanup is pending. Retry reads
  again; Skip asks for confirmation first. A run that planned no cleanup still
  passes the core's Cleanup phase for its final readback, but reports nothing
  pending, so it shows neither this row nor the cleanup-pending status.
- Retry: retries the running carry's step. A move that has committed changes the
  fingerprint the window opened with, so Retry uses the carry's own authority.
  While the resolver holds sync for an unfinished carry (`carry-recovery-required`)
  and no carry runs, Retry resumes that carry instead of starting setup.
- Resume unfinished change: shown when no carry runs and either the last carry
  stopped with its journal open, or the resolver holds sync for an unfinished
  carry. The second case covers a restart whose startup resume never ran.
- Check again does nothing while this window's carry runs, because it closes the
  window.

The status line shows progress, then the result: moved, cleanup pending or
skipped, paused here, stopped, unavailable or busy. A carry that is held but not
finished says it is waiting, with its reason. When a carry cannot confirm the
current mark (T5K's mark-winner-held and recovery-own-mark-held), it holds and
says the mark could not be confirmed, without claiming why. Its automatic check
says it is checking whether the move finished, never that the move is finishing,
because the check can end with finishing for Follow. After that finish, the
status says the settings were carried and another sync chat holds the newest
settings, then asks the user to open Sync chat and press Follow. Nothing follows
by itself: the Follow is the ordinary one, with its own consent, and the window
that ran the move does not offer it. An unreadable journal says nothing further
will be sent; it does not claim that nothing was sent before. The labels that say a change
is held for qualification appear only while that switch is OFF.

## Tests

- `test_sync_chat_carry_owner`: the owner with the real controllers, executor and
  setup flow, a fake transport and a persistent worker thread. The fake transport
  keeps T5K's contract: it renews every effect through the renewal hooks before
  answering, and answers Unknown without a permit. It covers moves
  into an existing chat and a new channel, cleanup ON and OFF, Skip and Retry,
  refusals, closing the window (including a late reply after it), crash and quit
  at every step boundary with startup resume, Follow and Pause,
  the service's startup resume (its real method bodies, sliced onto a probe) after
  a kill between the journal write and the mark: one that waits and runs on a
  status change with no activation, one that stops early on a busy lease and
  finishes once the lease is free, and the bound on repeated status changes, a Follow from an
  old chat with a late post of this device's under Cleanup ON and OFF, the
  owner's cleanup transport guard, recovery, a
  renewal answered empty when its window closes mid-hop, mark removal, and OFF
  per switch. A second pass reruns the ON operations with Cleanup held OFF and
  the other four ON, including a move that asks for cleanup and old-mark removal
  and a Follow from an old chat with a late post of this device's. Neither sends
  a cleanup command, and both finish.
- The compiled suites of the setup, move, cleanup, consent, mark, native and
  recovery controllers check each switch against its own constant. They expect
  the ON behavior when it is ON and the OFF behavior when it is OFF, so the
  build stays verified with any switch held back. The consent, mark and native
  suites check at the owner, the production entry: it admits Follow/Fill and
  mark removal, and its transport guard passes a cleanup command, only while
  that switch is ON. Each also keeps a control with only that switch held OFF.
- `test_sync_chat_carry_wire_routes`: the OFF guards of the wiring, with a
  negative mutation for each.

## Known limits

- Leaving the app, locking it and account changes are wired in the service and
  checked only by source review and the route test, not by a host test.
- The decoration of a newly created channel (pin, mute, archive) is not run.
- Turning Cleanup OFF after a build that had it ON can leave an unfinished move
  or Follow that still needs cleanup. Its resume then holds at cleanup with no
  Skip control, because the cleanup row is behind the switch. Holding Cleanup
  OFF first and turning it ON later never creates such a journal.
- These are host tests with injected native replies. They do not qualify the
  native app, the network or power loss.

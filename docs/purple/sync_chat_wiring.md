# Sync chat changes: how the setup screen runs them

A *carry* is one journaled change of where an account keeps its sync data:
a Change sync chat move, a Follow or Fill of another device's chat, a Lost
recovery, or the removal of one older Purple mark. The journal, its phases and
the executor are described in [move storage and recovery](sync_chat_move.md).
This page covers how Desktop starts, shows and stops carries.

All five feature switches are OFF in this build. Each one is a constant:

- Move: `DesktopSyncChatMoveActivated` in `purple_sync_chat_setup.cpp`;
- Follow/Fill: `DesktopSyncChatFollowFillActivated` in `purple_sync_chat_consent.cpp`;
- Mark removal: `DesktopSyncChatMarkRemovalActivated` in `purple_sync_chat_mark_actions.cpp`;
- Cleanup: `DesktopSyncChatCleanupActivated` in `purple_sync_chat_cleanup.cpp`;
- Recovery: `DesktopSyncChatRecoveryActivated` in `purple_sync_chat_recovery.cpp`.

With all five OFF, nothing on this page runs: the carry service is never
constructed, startup goes straight to ordinary reconciliation, and every control
keeps its existing OFF result.

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
journal first and runs ordinary reconciliation when it settles. If the app is
inactive or locked, reconciliation runs now and the resume waits until the app
is active again. With Move, Follow/Fill and Recovery OFF, it is ordinary
reconciliation alone.

A journal that cannot be read, or belongs to another account, is not treated as
absent. The resume ends held without creating or sending anything, and discovery
keeps sync held and reports it. The resume does not retry by itself.

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
- Check again does nothing while this window's carry runs, because it closes the
  window.

The status line shows progress, then the result: moved, cleanup pending or
skipped, paused here, stopped, unavailable or busy. A carry that is held but not
finished says it is waiting, with its reason. When a carry cannot confirm the
current mark (T5K's mark-winner-held and recovery-own-mark-held), it holds and
says the mark could not be confirmed, without claiming why; its automatic exit
belongs to the auto-recovery slice. An unreadable journal says nothing further
will be sent; it does not claim that nothing was sent before. The labels that say a change
is held for qualification appear only while that switch is OFF.

## Tests

- `test_sync_chat_carry_owner`: the owner with the real controllers, executor and
  setup flow, a fake transport and a persistent worker thread. The fake transport
  keeps T5K's contract: it renews every effect through the renewal hooks before
  answering, and answers Unknown without a permit. It covers moves
  into an existing chat and a new channel, cleanup ON and OFF, Skip and Retry,
  refusals, closing the window (including a late reply after it), crash and quit
  at every step boundary with startup resume, Follow and Pause, a Follow from an
  old chat with a late post of this device's under Cleanup ON and OFF, the
  owner's cleanup transport guard, recovery, a
  renewal answered empty when its window closes mid-hop, mark removal, and OFF
  per switch.
- `test_sync_chat_carry_wire_routes`: the OFF guards of the wiring, with a
  negative mutation for each.

## Known limits

- Leaving the app, locking it and account changes are wired in the service and
  checked only by source review and the route test, not by a host test.
- The decoration of a newly created channel (pin, mute, archive) is not run.
- These are host tests with injected native replies. They do not qualify the
  native app, the network or power loss.

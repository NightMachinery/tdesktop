# Desktop sync-chat service

`purple_sync_chat_service` is a thin account-scoped facade over Magic Purple Sync. `StartSyncChatService` observes account and stored-session changes, resolves each live session at startup and retires the session's Magic state when the session changes. Automatic sync starts after this service. The startup subscription includes `activeSessionChanges`, so already-stored accounts receive the same initialization.

`EnsureSyncChatResolved`, `CheckSyncChatBeforePost`, status and change notifications delegate to the account's Magic instance. Status carries its typed chat directly; no mark resolver or discovery cache remains. A post lease comes from `SyncMagicLease` and must still match the caller's account, environment, login and typed route. A borrowed operation capability remains valid only while its owner/store and current run are live.

`DesktopSyncWriteGate` preserves the account-local RAM write fence and epoch-bound writer leases. The dormant carry executor is gone; removing its name does not remove account exclusion or post admission. The ordinary upload, complete inventory and exact owned readback checks remain in [typed I/O](sync_chat_io.md).

The canonical host roster tests startup routes, typed facts and receipts, uploads, publisher file/account fences and first-use policy. It uses synthetic ports and makes no Telegram requests. See [Magic Purple Sync](sync_chat.md) for the user-visible behavior and compatibility boundary.

## Update-triggered pickup

New and edited messages in the bound chat schedule a check after 500 ms. A
burst shares one timer and a bounded set of message ids. The timer rechecks
the binding and uses core's issued-record filter after a local send settles,
so own post/edit acknowledgements do not feed checks back into themselves.
History reads do not enter these hooks. The existing pause, account, problem
and retry gates still decide whether a check runs. Lifecycle events may check
again after the fifteen-minute eligibility interval. An opted-in live account
also owns a fifteen-minute idle backstop, including before first-use state exists.
It uses the same event gates; delivered updates and the five-second save debounce
remain prompt. Each timer captures account/login, owner/file context and binding
authority. Disable, logout or disposal retires it, and late callbacks cannot
act on a replacement context. An eligible replacement gets a fresh delayed timer,
so an external context change cannot silently remove the idle backstop. It does not renew a stopped run's retry budget or
override a pending retry or write fence.

Foreground/resume rechecks eligibility and replaces an overdue timer instead of
replaying missed ticks. Sleep, suspension and process death cannot promise a
wall-clock delivery deadline. A chat update also rechecks first use when no state exists.

## Delivery when the sync chat stays closed

Mute and archive control notifications and list placement, not the message update
dispatch used here. Both clients recover gaps with the existing global/channel
difference paths. Their repeated channel short polls belong to opened chats;
this feature does not register the hidden chat for those polls.
[Telegram's update contract](https://core.telegram.org/api/updates#subscribing-to-updates-of-channels-supergroups)
says joined channels and supergroups usually receive passive updates, but the
server may stop or reduce them. Therefore pickup fires for a delivered new/edit
update while the chat is closed, but prompt delivery cannot be guaranteed.
Guaranteeing a shorter maximum delay would require polling or a different
transport, beyond this feature's no-faster-polling rule. Device checks must keep
the chat closed, hidden, muted and archived on the receiving client.

## First-use status in an open dialog

Automatic first-use completion updates an already-open Sync dialog when its
primary review still says the account is unavailable. The existing typed Joined
or Published result supplies the localized completion text; stale review actions
are discarded. Busy manual flows, other selected accounts, recovery journals and
real review errors keep their own status. Repeated notifications do not replay
an old completion. Missing startup-owner authority is described as an owner
selection problem, using the existing wording, rather than a missing-file error.

Automatic first use carries its own generation and automatic/manual ownership.
Disable retires automatic scans and publisher admission before persisting the
option, including when no state exists and chat lookup has not resolved. Old
callbacks cannot attach to a later run, publish, or apply. An explicit manual
first-use request keeps its own authority. Cancellation cannot undo a request
already accepted by the server; existing issued-write reconciliation remains.

The dialog refreshes automatic/chat controls on busy-state completion and account
loss as well as service notifications. A successful physical join or publication
clears a superseded executor error only after fresh recovery is bound without a
journal; current recovery failures and stored hold reasons remain visible.

## Fresh-login cache admission

The Magic chat binding cache creates its `sync/` directory with mode `0700`
and writes `chat.json` atomically with mode `0600`. Resolving an existing chat
happens before automatic sync or first-use review reads the account store.
This lets a newly logged-in device join that chat without a pre-existing
`state.json` or automatic-sync record.

Older cache writes used ordinary directory defaults, which could leave a
`0755` directory containing only `chat.json`. Recovery discovery rejected it
before reviewing any cloud records; manual Check could consequently report an
ambiguous local store. Cache initialization now tightens that directory only
when it is owned by this user, writable by nobody else, and contains just an
owned regular cache with one link. A directory containing state, pending data,
unknown entries or links is still refused. The state reader's private-mode
requirements and insecure-permission results are unchanged, including refusal
of directories missing owner read, write or search permission. On Unix the repair opens the directory without
following a replacement symlink, validates its identity and cache through that
descriptor, and changes permissions through the descriptor. A pathname swap
cannot change permissions on the replacement target.

The automatic-service host gate exercises the production binding file methods
on disk before enabling sync and joining an existing remote head. It covers
root and secondary accounts, a fresh cache and the older cache layout, matching
and customized settings, refusal of unsafe directory or link layouts, and a
disposable symlink swap immediately before descriptor chmod. The unrelated
target's original mode must remain unchanged.

## Applying after many saved versions

Before applying a reviewed update to an already bound account, the sync dialog
runs the existing initialize-only publisher worker. It checkpoints retained
config bases into private, durably confirmed `retained-bases/` files, retaining
the current base inline. It sends no records. Without this step, sixteen valid
inline bases filled the bounded store and a seventeenth update returned
`RetentionFull` before settings or History could be written.

The dialog uses its existing status label while preparing and keeps actions busy.
It requires the same session, generation and selected route when the worker
finishes. Native Apply then repeats the original review, account, binding,
settings fingerprint and store checks before retaining History and replacing
settings. Checkpoint failure leaves Apply refused. Existing retained records are
not dropped, the inline limit is unchanged and strict file admission is unchanged.
Synthetic host cases cover the native refusal, byte-for-byte preservation of all
sixteen bases, initialize-only completion without a post, Apply and Undo history.

## Refreshing an open sync dialog

The automatic-sync checkbox and status observe automatic-service, settings-file
and sync-chat changes. They re-read the current account on the next main-loop
turn, after the triggering operation has settled. This also refreshes the open
dialog after a manual first-use check changes local state. Destroying the dialog
discards queued refreshes; refreshes never enable sync, write preferences or
start a network request.

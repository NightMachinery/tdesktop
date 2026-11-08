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
again after the fifteen-minute eligibility interval. There is no periodic idle
timer, so an eligible idle client with no delivered update can wait longer. A chat update also rechecks first use when no state exists.

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

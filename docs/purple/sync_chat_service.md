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
and retry gates still decide whether a check runs. The fifteen-minute timer
remains the backstop. A chat update also rechecks first use when no state exists.

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

# Desktop sync-chat service

`purple_sync_chat_service` is a thin account-scoped facade over Magic Purple Sync. `StartSyncChatService` observes account and stored-session changes, resolves each live session at startup and retires the session's Magic state when the session changes. Automatic sync starts after this service. The startup subscription includes `activeSessionChanges`, so already-stored accounts receive the same initialization.

`EnsureSyncChatResolved`, `CheckSyncChatBeforePost`, status and change notifications delegate to the account's Magic instance. Status carries its typed chat directly; no mark resolver or discovery cache remains. A post lease comes from `SyncMagicLease` and must still match the caller's account, environment, login and typed route. A borrowed operation capability remains valid only while its owner/store and current run are live.

`DesktopSyncWriteGate` preserves the account-local RAM write fence and epoch-bound writer leases. The dormant carry executor is gone; removing its name does not remove account exclusion or post admission. The ordinary upload, complete inventory and exact owned readback checks remain in [typed I/O](sync_chat_io.md).

The canonical host roster tests startup routes, typed facts and receipts, uploads, publisher file/account fences and first-use policy. It uses synthetic ports and makes no Telegram requests. See [Magic Purple Sync](sync_chat.md) for the user-visible behavior and compatibility boundary.

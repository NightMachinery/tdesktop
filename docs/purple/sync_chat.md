# Desktop sync chat

## Magic Purple Sync

Each account syncs through one chat that every device finds or creates by
itself: a megagroup titled `Magic Purple Sync` that the user created and is the
only member and admin of. purple-core decides (`PlanSyncChatLookup`, see the
core doc `docs/sync-chat-magic.md`); purple_sync_magic_chat runs the requests
it asks for. It is a line-by-line port of Android's PurpleSyncMagicChat and
needs no app headers, so `cases-sync-magic-chat` runs it over a fake port.

- **Binding**: `sync/chat.json` in the account's sync directory, written by
  compare-and-set through `WriteGuardedSettingsFile`. It records the mode
  (magic or manual), the chat, when its full info was last read, the
  Telegram methods that must not be sent before a deadline, the creation
  journal, a problem and whether it was shown, and the list choice.
- **Lookup**: one at a time per account; a trigger that arrives while one runs
  shares its result. Triggers are startup, screen, manual, auto and post.
  The local candidates are the magic-titled chats in the main and archive
  dialog lists. With none visible, one `messages.getAllChats` proves absence
  before anything is created; the same reply settles a bound chat that is not
  in memory. Desktop's schema has no such method, so
  purple_sync_magic_telegram declares the request itself.
- **Create**: `channels.createChannel` (megagroup), then archive
  (`folders.editPeerFolders`, folder 1) and mute
  (`account.updateNotifySettings`). Archive and mute are cosmetic: a failure
  is logged and the lookup goes on. A lost reply blocks another creation for a
  day; the chat binds when it appears. A creation limit is a problem.
- **Verify**: `channels.getFullChannel` confirms the member and admin counts
  before a chat is bound, and again at most once a day before a post.
- **Waits**: a flood of up to 30 seconds is retried once in place. A longer
  one, a network failure, a timeout or a 5xx error records a deadline for its
  method in the binding (5, 10, 20, 40, then 60 seconds of backoff for the
  latter) and the lookup answers Wait. Every trigger honours the deadline,
  across restarts. Waits never pause automatic sync.
- **Problems**: a chat that is gone, left, shared, public, restricted or
  unpostable, a creation limit, magic-titled chats none of which is eligible,
  or another sync group's records in the chat. A problem pauses automatic sync
  until the user acts and is shown once; a manual check clears it and decides
  afresh.

purple_sync_magic_service keeps one lookup per session and maps its result onto
the facade's `DesktopSyncChatStatus` (Use, Searching for a wait, Paused for a
problem). Its lease builds the typed route for Saved Messages or the bound
supergroup; a magic chat verified by full info counts as read only by the
user. The facade functions `EnsureSyncChatResolved`, `CheckSyncChatBeforePost`,
`SyncChatStatusOf`, `SyncChatIoForPost` and `SyncChatStatusChanges` now answer
from it. The search-based discovery below stays compiled for the old setup box
until that box is rewritten, and is then deleted. The launch-time search of
Saved Messages for a newer settings file is gone.

## Discovery (being retired)

The Desktop M2B4 adapter uses the shared sync-chat ownership, eligibility and
resolution contracts. Source preparation and unit tests do not establish native
search coverage or authorize Telegram requests, setup or posting.

### Native facts

purple_sync_chat_facts maps the generated Desktop TL message, user, chat and
full-info objects. Every snapshot has an explicit account environment, bare
user id and per-login identity. The message target must normalize to the same
typed chat as its snapshot. Equal numeric ids of users, groups and channels
never alias. A new login or environment mismatch produces unknown ownership.

The mapper supplies the C0 tri-state ownership predicate, rather than converting
its boolean result back into Own or Other. All common message exclusions are
mapped, including via-business/guest bot provenance, scheduled/repeating posts
and document/photo TTL. Unsupported media with unknown TTL remains unknown.
Saved Messages does not require outgoing or sender flags. Group messages require
a typed self sender; broadcast ownership requires outgoing plus affirmative
creator and sole-admin evidence from the matching full-info object. Normal
broadcast attribution to its channel is not alternate send-as. Broad outgoing
broadcast acceptance is disabled.

Eligibility is separate from ownership. Missing/minimal peer metadata, missing
or mismatched full info, unknown file rights and unknown chosen send-as hold
eligibility. The caller must obtain current rights and chosen send-as from the
same origin session after full-info processing; they are not inferred from
Desktop's optimistic reader/admin defaults. Saved Messages also requires its
matching self user/full-info snapshot. Full-info TTL, paid-message and protection
facts are checked. A private recipient's discoverable username does not make
that private conversation publicly readable; group/channel public usernames
are refused, including active secondary usernames.

Basic-group reader counts come from the full participant list with matching
chat id, unique positive ids and explicit self membership. Forbidden/incomplete
rosters stay unknown and require a reader warning. Channel counts come from
full info, never the Desktop ChannelData default admin count. A missing broadcast
admin count cannot establish sole-admin status. Reader snapshots are local
facts and do not claim that a roster of accounts is synced.

### Source and qualification boundary

The mapper and core C0 are compiled directly by purple/test_sync_chat_facts.py.
The fixture constructs real generated TL objects and reuses the pinned schema
and base libraries from an existing Debug tree. It builds a standalone unit
program, not Telegram. No network sender, profile or GUI is constructed. The
input manifest records source/schema/object hashes. Native N1/N2 acceptance
still needs a later GO and real origin-bound services. The discovery executor,
cache, service and startup/Sync wiring are a separate T1b layer.

R2 remains required until Desktop N1 self-parity qualifies it. Ordinary R1 must
include archived chats, use the empty filter and consume bounded pages; the
caption/document limit-one settings hint is not inventory. Incomplete scans and
unknown exact reads never prove absence. Automatic settings sync remains
Self-only, and established-stream moves remain held for carry execution.

## Diagnostics

Sync writes structured lines to log.txt as `Purple diag: <area> <event>
key=value...` through purple_sync_diag. Areas, events, field names and
enumerated values must be words of its vocabulary; anything else is written
`invalid`, and counts and seconds are clamped, so an id, a chat title or other
free text cannot reach a line. The vocabulary matches Android's
PurpleDiagLogStore words for the `sync_chat` area.

A repeat limit keeps a failure that repeats thousands of times from flooding
the log. Each distinct line body may be written 3 times per 10 minutes; later
copies are only counted. When the window ends, when the body is evicted from
the 64 most recently seen ones, or when the clock goes backwards, one
`diag repeated area=<area> event=<event> count=<n>` line reports the dropped
copies. StartAppServices installs the log.txt sink; earlier lines are dropped.
The gate `cases-sync-diag` (purple/tests/sync_diag_cases.cpp) checks the
vocabulary, the line shape and the limit.

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
  journal, a problem and whether it was shown, the list choice, and the
  candidates that failed verification.
- **Lookup**: one at a time per account; a trigger that arrives while one runs
  shares its result. Triggers are startup, screen, manual, auto, post and
  retry (the timed wake-up after a wait, never taken for a user's check).
  The local candidates are the magic-titled chats in the main and archive
  dialog lists. With none visible, `messages.getAllChats` proves absence
  before anything is created; the same reply settles a bound chat that is not
  in memory. Desktop keeps no local chat database, so that costs one request
  per process, after which the reply's chats are in memory. A reply the
  server sliced (`messages.chatsSlice`) is paged with `except_ids`, up to five
  pages; a list still incomplete proves nothing absent, so the lookup never
  creates from it and waits an hour (`list_partial`), though a magic chat on
  a page is still found. Desktop's schema has no such method, so
  purple_sync_magic_telegram declares the request itself.
- **Create**: `channels.createChannel` (megagroup), then archive
  (`folders.editPeerFolders`, folder 1) and mute
  (`account.updateNotifySettings`). Archive and mute are cosmetic: a failure
  is logged and the lookup goes on. A lost reply blocks another creation for a
  day; the chat binds when it appears. A creation limit is a problem.
- **Verify**: `channels.getFullChannel` confirms the member and admin counts
  before a chat is bound, and again at most once a day before a post. Another
  candidate that fails (an older magic chat someone joined, say) is only
  rejected: core keeps it in the binding and skips it, so sync goes on in the
  bound chat and an unbound lookup tries the next one. A manual check
  considers rejected chats again.
- **Waits**: a flood of up to 30 seconds is retried once in place. A longer
  one, a network failure, a timeout or a 5xx error records a deadline for its
  method in the binding (5, 10, 20, 40, then 60 seconds of backoff for the
  latter) and the lookup answers Wait. Any other refusal of getAllChats,
  createChannel or getFullChannel (`PEER_FLOOD`, an unexpected 400) is kept
  the same way for ten minutes, doubling up to a day. Every trigger honours
  the deadline, across restarts. Waits never pause automatic sync.
- **Sync request waits**: a flood over 30 seconds on a record read, post,
  edit, deletion or upload sent through `SyncRequestSender` is kept in the
  same binding under its method (`wait_recorded`), and every sender holds
  that method's requests until it ends, after a restart too. The emoji
  exporter, which hands long waits back to its own queue, keeps none.
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

Automatic sync follows the bound chat. When it opens the account's store and
the lookup has bound a chat other than the one the state names,
`DesktopSyncAutoStore::MoveTo` moves the state there: it writes the automatic
record first (core `MoveSyncAutoRecord`: the step in flight is dropped and the
settings file on disk counts as saved), then the state (core
`MoveSyncStateToChat`), and last deletes a config stage that was still pending
in the old chat, which is abandoned because that chat is no longer read.
Opening the store removes such a stage if an interrupted move left it behind.
The next run finds its own head missing from the new chat and publishes the
local settings there with the old base as parent. A refused move leaves the
state where it was, and automatic sync then holds instead of using the old
chat. Each move writes one `sync_chat migration` diagnostic line.

The move does not depend on automatic sync being on. It also runs when the
lookup reports a new bound chat (`MoveSyncStateToBoundChat`), opening the
store only for the move and closing it again.

The automatic facts call the chat resolved when the lookup's chat equals the
state's chat, and report a lookup problem, which pauses automatic sync
(`SyncChatProblem`). Check, read, post, edit and retire all use the lease for
the state's chat; a retire in a supergroup uses `channels.deleteMessages`.
When a complete scan of the chat finds records of other sync spaces only,
automatic sync raises `space_mismatch` in the binding and pauses until the
user acts. Send after Save posts to the bound chat (Saved Messages or the
Magic Purple Sync chat); the explicit Send to Saved Messages action still goes
to Saved Messages.

The Sync chat box (purple_sync_magic_box, opened from the Sync screen; the
pure frame and pick policy are `SyncMagicFrameOf` and `SyncMagicPickOf`)
shows which chat this device syncs through, a Telegram wait with its end, or
the problem that paused automatic sync. A problem not shown before gets one
dialog, in the box or on the Sync screen, and is then marked shown in the
binding, so it is not shown again until it changes. Check now runs a manual
lookup, which clears a recorded problem and decides afresh. Open sync chat
opens the bound supergroup. Show sync chat in chat list sets `listed` in the
binding; without it Purple's chat lists hide the magic chat, never a chat the
user picked. Use another chat lists Saved Messages and the supergroups in the
local main and archive lists, with no search, and accepts only Saved Messages
or a supergroup the user is in alone, judged from the cached member count
(owner decision Q2); the lookup then checks it like any manual binding. Use
Magic Purple Sync forgets the pick and keeps the waits and the list choice.
For `space_mismatch` the box offers Join: `SyncLocalStore::SetAside` moves
the pending stages, the automatic record, the retained bases and last the
state into a new `orphaned-<date>` folder (nothing is deleted), the problem is
cleared, and the Sync screen's next check offers to join the records already
in the chat. The old setup box (purple_sync_chat_setup_box) is no longer
opened; it is deleted with the rest of the search-based discovery.

The magic chat is a bookkeeping chat, so Purple leaves it out of the main
list, the archive, folder tabs, Work Mode views and the forward picker, with
or without a preset, unless the user turned on Show sync chat in chat list. A
chat the user picked is never hidden, and search and links still open it.
`History::purpleHiddenFromChatList` asks `SyncMagicHidden`, which answers
from a per-session map and returns at once for any peer that is not a channel.
The magic service updates the map whenever the lookup or the binding changes
(created, bound, listed, picked, signed out) and re-evaluates the old and new
chat's list rows at once.

Purple builds from before Magic Purple Sync keep reading and posting in Saved
Messages, so a device that is not updated drifts apart from the updated ones
until it is (owner decision Q5).

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
unknown exact reads never prove absence. Automatic settings sync follows the
chat Magic Purple Sync bound (above), and established-stream moves remain held
for carry execution.

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

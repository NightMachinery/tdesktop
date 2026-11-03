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
  Startup runs for every session when the active session changes, because
  upstream loads stored accounts without announcing them.
  The candidates are the magic-titled supergroups the user is in, read from
  every loaded chat in the session and not from a chat list, so a Work Mode
  preset that hides chats everywhere cannot hide one from the lookup. Only
  complete lists prove absence: the main list (upstream loads it in full)
  and the archive folder, which the lookup pages through upstream's own
  `requestDialogs` because upstream loads one archive page. An account with
  no archive folder counts its archive as loaded once the main list is. The
  wait ends as soon as a bound chat is loaded (`SyncMagicListsStepOf`) and
  lasts at most 60 seconds. Until both lists are complete, no chat is created
  and a bound chat missing from memory is not called gone; the lookup answers
  a short wait of Purple's own (`lists_loading`, shown as the Looking line)
  and looks again a minute later. Magic chats in view that are all unusable
  or rejected start the same wait; only complete lists make them
  `magic_ineligible`. With complete lists and no magic chat it creates one;
  a bound chat still missing is gone. No request outside upstream's layer is sent:
  `messages.getAllChats` is not in layer 232, and the server refuses it.
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
- **Waits and refusals**: core's `ClassifySyncChatError` sorts every failed
  lookup request.
  - Only FLOOD_WAIT, FLOOD_PREMIUM_WAIT and SLOWMODE_WAIT are Telegram's waits.
    A flood of up to 30 seconds is retried once in place. A longer one records
    a deadline for its method in the binding, which every trigger honours
    across restarts. Only these waits are worded "Telegram asked Purple to
    wait".
  - A timeout, a network failure, a 5xx or a 401 is transient: Purple backs
    off 5, 10, 20, 40, then 60 seconds in memory and says only when it looks
    again.
  - Any other error is a refusal, a problem (`request_refused`, or
    `create_limit` / `chat_gone` where core names one). For a candidate other
    than the bound chat it only rejects that candidate.
  - Waits never pause automatic sync.
- **Sync request waits**: a flood over 30 seconds on a record read, post,
  edit, deletion or upload sent through `SyncRequestSender` is kept in the
  same binding under its method (`wait_recorded`), and every sender holds
  that method's requests until it ends, after a restart too. The emoji
  exporter, which hands long waits back to its own queue, keeps none.
- **Problems**: a chat that is gone, left, shared, public, restricted or
  unpostable, a creation limit, a refused request, magic-titled chats none of
  which is eligible, or another sync group's records in the chat. A problem pauses automatic sync
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
lookup reports a new bound chat and when the Sync screen's Check starts
(`MoveSyncStateToBoundChat`), opening the store only for the move and closing
it again. The Sync screen and the publish flow take the bound chat's lease
and the binding file as their authority; the legacy setup snapshot and
discovery cache are checked only when they exist. A first publish to a
verified magic chat joins the space core selects there (the earliest one), even
when the chat also holds the records of a device that lost the race to start it.

The automatic facts call the chat resolved when the lookup's chat equals the
state's chat, and report a lookup problem, which pauses automatic sync
(`SyncChatProblem`). Check, read, post, edit and retire all use the lease for
the state's chat; a retire in a supergroup uses `channels.deleteMessages`.
When a complete scan of the chat selects a space other than the state's,
automatic sync runs first use (below) instead of pausing. Send after Save posts to the bound chat (Saved Messages or the
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
Automatic sync no longer raises `space_mismatch`; a binding that still
records it from an older build keeps the box's Join until a manual check
clears it. For `space_mismatch` the box offers Join: `SyncLocalStore::SetAside` moves
the pending stages, the automatic record, the retained bases and last the
state into a new `orphaned-<date>` folder (nothing is deleted), the problem is
cleared, and the Sync screen's next check offers to join the records already
in the chat. The old setup box (purple_sync_chat_setup_box) is no longer
opened; it is deleted with the rest of the search-based discovery.

### First use

A device acts on the bound magic chat for the first time while it has no
state in the chat's space: a fresh login or install (no `state.json`, recovery
status Setup), or a state in another space, such as the loser of two
customised devices racing into an empty chat, or D1's Desktop after Android
started a fresh space. `ReviewSyncFirstUse` (purple_sync_config_review) reads
the local file and the store, calls core's `SyncFirstUseInputOf` and
`PlanSyncFirstUse`, and returns the need (`NoState` or `OtherSpace`), the
action, the publish request (the local fingerprint, every head of the selected
space as parent) and the single head's key. A device whose own pending stage
waits with no state, or whose inventory core cannot plan from, gets no action
and the usual named refusal.

It runs from two places, both through `RunSyncFirstUse` in the automatic
service: the automatic service, when an enabled account cannot open its store
because it has no state (`NoStateEvent`) or when a check of an open store
finds the state outside the selected space (`PauseOnForeignSpace`); and the
Sync screen's Check, before its usual review. An unfinished Start again
archive (a recovery journal) refuses it. Enabling or disabling automatic sync
on a device with no state writes the switch beside the missing state
(`SetDesktopSyncAutoBeforeState`), so the store opens enabled once first use
creates it. Without a state the store's 15-minute lifecycle limit cannot
run, so the mark also keeps the time and local fingerprint of the last
outcome: a lifecycle event (session ready, foreground, unlock, retry) starts
no new scan within 15 minutes of it. Enable, a sync chat status change, a
manual Check, and a settled save that changed the file bypass the limit; a
save still in its five-second delay waits for it.

- Wait (empty chat, default settings): nothing is sent. The Sync screen says
  it waits for a device with your settings; the automatic status says so too.
  A local file that does not load, or a last-good copy in use, is named as a
  file error instead and needs attention.
- Publish (empty chat, customised settings, or the answer "Use this device's
  settings"): the existing publisher posts the first record, or the successor
  of the chat's heads, with no confirmation.
- Join (default settings, or the answer "Use the synced settings"): the
  publisher initializes the state in the selected space without sending, then
  `ApplySyncFirstUseJoin` applies the chosen head through the manual Apply
  path, with History and Undo for the previous file.
- JoinSilently (the same meaning as the chat's one head): the same join; the
  local file is never touched. When the bytes differ although the meaning is
  the same, Desktop publishes them as the successor.
- Ask (different customised settings): one choice on the Sync screen, "Use
  the synced settings" or "Use this device's settings", and an attention line
  in the automatic status. No dialog opens by itself. The answer is fed back
  as `SyncFirstUseChoice`; the state it creates in the chat's space settles
  it, so the question never comes back.

The answer and a join in progress are kept beside the state, in
`sync/first-use.json` (`SyncFirstUseMark`), written before the archive or the
publisher runs. A run that dies after the answer applies the same answer next
time instead of asking again. A join that dies after the publisher wrote the
new, still unjoined state is resumed (`SyncFirstUseNeed::Unjoined`): the head
is applied without a second join, so a fresh login never sees a conflict
review. The mark is removed once a publish or join succeeds, or once the state
in the chat's space has a base.

Before acting on `OtherSpace`, the old state is archived with the Start again
machinery (`StartAgainSyncAccount` with `firstUse`, planned by
`PlanSyncFirstUseArchive`): the state and complete pending stages move to
`sync/orphaned-<date>/`, while History, the settings file and the chat binding
stay. The device then acts as an unbound one.

Pre-magic carry leftovers (`carry.json`, `carry-cache.json`, `carry-objects`,
`carry-history`) would otherwise hold the store's startup admission
(`carry-recovery-required`). While a magic chat is bound and no carry posting
is held, every store open and every first use first moves them, under the
account lock, into a new owner-only `sync/carry-retired-<date>/`
(`RetireSyncCarryLeftovers`); nothing is deleted. Each step writes one
diagnostic line: `sync_chat first_use trigger=<auto|manual> action=<...>
choice=<...>`, then `step=archive|publish|join|apply outcome=ok|error`, and
`sync_chat migration step=carry` for the retirement. A first use that fails
writes `sync_chat first_use outcome=error reason=<code>` and shows one line
for that code: `busy`, `unbound`, `chat_unavailable`, `incomplete`,
`account_changed`, `recovery_journal`, `unreadable`, `files_unwritable`,
`archive_refused`, `settings_invalid`, `chat_changed`, `publish_unconfirmed`,
`publish_refused` or `apply_refused` (with the apply's own text). When core
cannot plan from the inventory, the review's own reason code and refusal text
are used.

The host cases are FirstUseActions, FirstUseD1, FirstUseMigration and
FirstUseRace in `purple/test_sync_config_publish.cpp` (gate setup-publish), and
FirstUseJoinAppliesHead, AutoBeforeState and MigrationKeepsSharedSpace (two
installs that adopted one head in an old chat both move, one republishes, the
other's next run settles with Adopt or UpToDate and no conflict) in
`purple/test_sync_auto.cpp`. FirstUseMarkKeepsAnswer covers the mark.
The routes gate (`purple/test_sync_chat_routes.py`) pins the triggers, the
journal guard, the carry retirement, the actions and the Sync screen's choice.

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

Every NeedsReview refusal of a sync check, from the box or from automatic sync,
writes `sync_chat review_refused trigger=<manual|auto> reason=<code>`, with
core's `SyncConfigReviewReasonCode`: `space_mismatch`, `unreadable`,
`ambiguous`, `newer_schema`, `heads_unclear` or `own_records` (`unknown` if a
refusal has none). The box shows one line per reason instead of a generic
sentence. Desktop's own refusals name theirs too: a newer local file or a head
Desktop cannot apply (`newer_schema` or `unreadable`), an ambiguous or
unreadable recovery inspection, and a state that names another chat
(`space_mismatch` when the chat holds another space, else `own_records`).

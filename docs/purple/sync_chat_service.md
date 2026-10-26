# Desktop sync-chat service

The service resolves a sync chat for one captured account and login. It performs
read-only discovery. It creates no chat, mark, pin, post or dialog. Native server
qualification and outward execution require a later GO; the standalone unit
program does not construct a Telegram session or send requests.

`StartSyncChatDiscovery` subscribes to account/session startup before automatic
settings sync starts. The existing Saved Messages startup hook calls
`EnsureSyncChatResolved`; opening the Sync screen requests a fresh resolution.
`SyncChatStatusOf` and `SyncChatStatusChanges` expose Unset, Searching, Use,
Paused and Lost, the core resolution, a held reason and retry deadline. The
chooser and reader-consent UI belong to the setup slice. Background failures
publish status and never open a box.

## Requests and evidence

R1 uses ordinary global search for `#purplechatsync`, the empty filter, limit
100 and no folder flag, so archived chats are included. Pagination carries the
server rate, typed offset peer and message id. Counts must remain stable;
unknown constructors, inexact/search-flood pages, cursor loops and contradictory
counts are incomplete. A scan consumes at most 20 pages. A capped global scan
tries broadcasts, then the bounded F1 list of up to 50 already loaded main and
archive dialogs. Creator channels named Purple sync come first, then other
creator channels and groups by recency. F1 does not load all server dialogs or
establish cache-free coverage. Retained R2 also searches Self. No first-hit
shortcut is used: supported marks are normalized, sorted and deduplicated by
the shared resolver.

R3 rereads an exact mark with `channels.getMessages` for a channel and
`messages.getMessages` otherwise. It refreshes matching peer/full-info facts
and obtains separate history-readability evidence. Missing or edited marks
become MarkGone only with affirmative readable-chat evidence. Two qualified
CHANNEL_PRIVATE refusals on the channel exact-read and independent history
paths can establish ChatGone. Flood, network, wrong constructor, contradictory
id/peer/date and unsupported migration evidence remain Unknown. Unknown keeps
the previous read chat and holds posting. A newly selected winner is always
reread directly before resolution can proceed.

Q searches Self for caption `#purplesync` with the document filter and limit
one only when there is no effective cache or eligible mark. It is a hint,
followed by complete inventory and validated record bytes. An empty, excluded
or valid first Q hit always proceeds to full inventory. Q alone never proves
absence or presence. Unknown Q evidence holds resolution. The C0 tri-state
ownership filter is applied to both scanning and fresh candidate metadata:
Unknown makes inventory incomplete, Other cannot become a candidate, and only
Own proceeds. Existing legacy inventory callers keep their prior behavior;
the explicit typed constructors carry the same scope through both stages.
See [typed I/O](sync_chat_io.md) for endpoint and ledger rules.

Modern history validates the full response before extracting messages.
Exact channelMessages is also admitted for a typed Channel route. Otherwise
only exact ordinary messages or exact slices are admitted; inexact, search-flood,
negative or changing counts, wrong typed targets, stalled cursors, unfinished
pages, transport failures and the 20-page cap remain Unknown. An empty page with
an unsatisfied declared count cannot prove completion. The service inventory
reply mapper also requires complete scan and read statuses, the captured account,
and exactly one validated read result per scanned candidate. A malformed or
incomplete result cannot be converted to Empty or release posting. The previous
cache remains available when resolution is held.

## Cache, authority and serialization

The private device-local cache is named
`purple-sync-chat-<environment>-<user>.json` under the configuration root.
It stores a typed chat, mark id/date, display title, local reader facts and
consent, plus preserved future fields. A present carry object holds discovery;
this slice cannot execute an established-stream move. Follow, Fill and winner
inventory retain the original cache and never commit a moved state or cache.

Cache admission also requires a purpose-specific persisted login nonce from
account-local encrypted preferences. Selection reuses the reviewed B3 rule:
a persistent auth-key id set must intersect the current set, across every DC,
then the set is rebound while keeping the nonce. A disjoint login gets a fresh
nonce. The cache contains a domain-separated SHA256 binding, never the nonce
or raw key material. A RAM UUID, run number and ticket independently fence
callbacks. No login token is logged. Upstream exposes persistent key ids through
`MTP::Instance::getKeysForWrite` (`mtproto/mtp_instance.h:89`) and
`MTP::AuthKey::keyId` (`mtproto/mtproto_auth_key.h:36`); the selected main DC
is not the identity. Account-local preferences are destroyed with upstream
logout storage (`main/main_account.cpp:555` calls `local().reset()`, and
`storage/storage_account.cpp:772` clears the preferences). Cache mismatches, malformed/newer files and missing authority
refuse without deleting or silently replacing the file.

Cache reads/writes and lock operations run on the worker. The lock is exactly
the existing account sync directory's `lock`. The worker verifies exact cache
and settings bytes and identities before a commit. It has no good-file fallback.
`CheckSyncChatBeforePost` can borrow an existing account-operation lock only
with the captured account, exact sync directory and a current-operation proof.
Without that proof it acquires the same lock itself. Completion releases its
own lock before publishing status. The GUI never waits for worker I/O.

Settings/source, Work Mode, binding or login changes retire a run and cancel
its session-owned sender. A callback with a superseded run/ticket/origin cannot
cancel newer work. A still-owned callback that loses authority stops, publishes
a held reason and schedules its lock release. Each active request step has a
30-second timeout, which pauses rather than inferring absence. Searching retries
at 30, 120 and 600 seconds only after affirmative mark/chat loss. After that the
status is Lost. Unknown failures have no periodic polling loop.

## T2 and native boundaries

`ReadWinnerInventory` is the typed result seam. Complete, validated Present or
Empty results update the resolver's evidence and resolve again with the
original cache. Production inventory now uses explicit typed readers and
downloaders, with fresh matching facts and the captured run's current proof.
It refuses an unavailable typed endpoint instead of falling back to Self.
Complete non-Self unit inventories are controlled evidence, not native server
coverage. `CheckSyncChatBeforePost` remains the fresh-resolution interface for
writer integration; typed I/O does not bypass the publish gates.

Automatic settings sync additionally requires a resolved, unheld Self result.
Its existing core Self-only guard remains intact. This slice does not activate
automatic non-Self sync or allow Follow/Fill to post.

## Focused checks

Run the standalone units using an existing Debug tree:

```sh
python3 purple/test_sync_chat_facts.py --build-dir EXISTING_DEBUG_TREE --output-dir UNIT_DIRECTORY --build-only
python3 purple/test_sync_chat_discovery.py --build-dir EXISTING_DEBUG_TREE --output-dir ANOTHER_UNIT_DIRECTORY --build-only
python3 purple/test_sync_chat_completeness.py --build-dir EXISTING_DEBUG_TREE --author-unit-dir ANOTHER_UNIT_DIRECTORY --output-dir COMPLETENESS_DIRECTORY
```

Hold the compiler only during each compile step, then release it before running
the produced executable. These commands compile small unit programs and reuse
recorded generated schema/base objects. They do not build Telegram. The discovery
fixture also reuses private-file and unchanged core-state helpers, recording
object hashes. Literal ports execute the production callback and ownership
branches with substituted session, notification and worker endpoints. Real-file
cases use synthetic private temporary directories and retain their evidence.
They do not open an account/profile or establish native acceptance.

The completeness fixture passes literal full-response scanner callbacks through
the real service inventory-reply mapper into the discovery model. It covers an
excluded first Q hit followed by an older owned record, a valid first Q hit,
exact Empty, and incomplete/flood/capped/failed inventories. The preserved
`test_sync_chat_review_t1.cpp` is the original independent review source. Its
transport stub receives only the added production guard member in a derived
translation unit; assertions are unchanged. Its `InexactInventory` scenario drops
the inventory status and feeds Empty for both exact and inexact results. Its two
held-status assertions therefore remain failing harness evidence, separate from
the passing end-to-end fixture. The task report records the exact source lines.

Native source syntax checks must replace a reused recipe's `LANG_KEYS_SUBSET`
for the actual unit. A different file's subset causes misleading missing-string
errors. Use generated headers only in the unit workspace; do not rewrite a
native build tree for source-only checks.

## Before-post policy and reader warning state

Every before-post check rereads the exact cached mark through R3 and obtains
fresh full info, eligibility and reader facts. A complete, unheld discovery
result can avoid another R1/R2/Q/inventory cycle for less than 15 minutes. Its
RAM receipt matches the account, environment, login, settings root, owner and
Work Mode, binding token, settings space/chat, and cached mark. A carry, restart,
changed scope, incomplete result, failed save or backwards clock cannot reuse
it. Fast checks do not extend that deadline. Expiry while a direct check is
running restarts full discovery before completion is delivered. Existing account
locks, current-operation proofs and unknown/lost-mark posting holds remain.

Full record inventory has its own 1000-page limit. Reaching that page stays
Unknown, even if the response also looks terminal. R1 global mark search keeps
20 pages. Inexact, flood, changed-count or unfinished evidence still holds and
retains the original cache.

Known new readers retain prior consent for the same chat and set a sticky,
device-local `new_readers` cache/status flag. The original reader baseline stays
available. Available small basic-group rosters are retained as sorted,
account-scoped member digests; an added id, a higher count when no list is
available, or loss of creator rights sets the warning. Departures alone do not.
Channels without a roster retain the count-based blind spot: replacement at an
unchanged count cannot be detected. Unknown populations and lost posting rights
still hold. A previously unknown baseline is initialized when facts become
known. Optional warning/snapshot fields accept old caches, while malformed new
fields refuse. No discovery-freshness receipt is persisted across restarts.

This policy/state patch adds no presentation. T3 owns Sync screen warnings and
controls. It activates no setup or automatic non-Self writes.

The compact unit fixture compiles actual model, mapper, cache, request and disk
code. It uses literal service scope and existing upload/read callback bodies with
controlled transport endpoints, and verifies complete inventories beyond20pages,
1000-page refusal, exact R3 outcomes, scope/expiry, real staged-store changes and
warning persistence. Reused support objects require matching source/object hashes:

```sh
python3 purple/test_sync_chat_alignment.py --build-dir EXISTING_DEBUG_TREE --output-dir UNIT_DIRECTORY --reuse-unit-dir QUALIFIED_TYPED_UNIT_DIRECTORY --build-only
UNIT_DIRECTORY/test_sync_chat_alignment
python3 purple/test_sync_chat_alignment_routes.py
```

These source/unit cases do not qualify native Telegram, app lifecycle or UI.

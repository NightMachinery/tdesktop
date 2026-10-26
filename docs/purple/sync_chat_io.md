# Typed sync-chat I/O

The modern read adapters take `DesktopSyncChatIo`: one captured account,
environment and login, a canonical typed chat, its matching native InputPeer,
and fresh mapper facts. Unknown eligibility, missing access hashes, aliases of
Self and mismatched input types or ids refuse before a request. A caller also
supplies an observer-free current-operation predicate. Polling that predicate
must not cancel or destroy the adapter. The adapter checks its captured session
and account-settings target before requests and callbacks, and again after a
download. Logout, Work Mode, source or run replacement makes the work incomplete.
No active-account fallback is used.

## Read route

The explicit typed constructors of `SyncHistoryScanner`, `SyncCandidateReader`
and `SyncAccountInventory` share one unchanged route. History uses that chat's
InputPeer. Exact channel reads use channels.getMessages with its matching
InputChannel; Self, User and basic Chat use messages.getMessages. Returned
message id and typed peer are checked independently of request construction.
Unknown constructors, inexact/flood or contradictory counts, wrong target/id,
transport failure and capped or unfinished scans cannot prove absence. Modern
history retains the 20-page bound and checks exact terminal evidence before it
can be Empty. Q remains a hint before complete inventory.

`DesktopSyncChatInventory` carries the route beside the unchanged core inventory.
The service checks the captured origin and exact typed chat before converting
it into resolver evidence. A naked core result or a matching numeric message
id from another chat is insufficient. Follow, Fill and winner-inventory work
still preserve the original cache and never commit a moved stream.

Fresh candidate metadata applies C0 again before download. The file-reference
refresh origin is FullMsgId(actual typed peer, message id), and the downloader
uses the captured account. The received body must have exactly the declared
bounded document size. Cancellation, loader error and incomplete bytes refuse.
The typed exact-read helper checks metadata, downloads the record and rereads
metadata in the same chat; document id/edit date changes keep the result unknown.

## Ledger and compatibility

A message id in the local ledger belongs to the enclosing store's chat. Version
1 implies Self; version 2 must match its explicit chat. Store readback confirmation
and absent-entry retirement require that exact chat before touching the ledger.
Their default argument is Self, so an old caller cannot consume a non-Self v2
id accidentally. The core's existing current-head deletion refusal is retained.
No schema migration or per-message chat field is added.

The original constructor overloads remain explicit Self paths for existing
legacy callers. Raw inventory-based review/publishing refuses a non-Self v2
store. The typed manual publishing seam is a separate route; its caller must
carry the inventory route through the existing binding, recovery, staging,
queue and fresh before-post gates. This does not execute an established-stream
move or supply setup UI.

## Qualification boundary

`purple/test_sync_chat_io.py` builds a small standalone program against the
existing generated TL schema. It compiles real route helpers, metadata, model
and store code, and ports the literal full scanner, candidate, download and
exact reread callbacks. Session, transport and downloader deliveries are
controlled unit endpoints. Real-file cases use retained synthetic directories.
Request wire, channel identity, complete-only Empty, declared sizes, retirement
and wrong-chat ledger controls do not establish server or UI acceptance.

Compile with compiler:local, release it, then run the output executable:

```sh
python3 purple/test_sync_chat_io.py --build-dir EXISTING_DEBUG_TREE --output-dir UNIT_DIRECTORY --build-only
UNIT_DIRECTORY/test_sync_chat_io
python3 purple/test_sync_chat_routes.py
```

Native server calls, posting, setup execution and full Desktop builds require a
later GO. Automatic sync remains Self-only in the core and service. Typed manual
I/O support cannot enable automatic non-Self posting.

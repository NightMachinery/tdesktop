# Moving a settings sync chat: local storage

T5a provides storage only. It adds no network executor, screen, background move,
or automatic non-Self posting. Carry transports and foreground consent arrive
in later slices. The shared carry planner and journal remain authoritative.

## Retained base format, shared by clients

The shared local state remains version 1 (implicit Self) or version 2 (typed
chat). There is no Desktop version 3 and no core codec change. The core preserves
an optional top-level `retained_config_bases` object alongside its known fields:

```json
{
  "retained_config_bases": {
    "version": 1,
    "records": [
      { "sha256": "<64 lowercase hex digits>", "record": "<canonical record JSON>" }
    ]
  }
}
```

`record` is a JSON string containing the complete canonical UTF-8 settings
record, including its envelope, exact settings text, causal version and lineage.
`sha256` hashes those UTF-8 bytes, before JSON-string escaping, using SHA-256.
The record must parse as a supported settings record and its space must match
this state. A base lookup requires both the exact version key and exact lineage.
A matching text fingerprint alone is insufficient. An adopted remote version
can be a base; its writer does not become this install's writer.

Each record is at most 262144 bytes (256 KiB), measured before string escaping.
There are at most 16 distinct hashes and at most 2097152 total record bytes.
The complete serialized state must also fit the existing 4194304-byte core
limit. These are independent bounds: JSON escaping can exhaust the state limit
before the raw-record total. Oversize records return `BaseRecordTooLarge`;
count, total or encoded-state exhaustion returns `RetentionFull`. Neither path
truncates records, drops unknown fields or partially updates the state.

Version must be a JSON number exactly 1. Numeric future versions, including
2147483648, return `NewerRetention`; missing/invalid required members, duplicate
hashes, bad hashes or invalid records return `InvalidRetention`. Unknown container
and record-entry members are preserved. A missing subobject is a legacy state,
not an empty retained base. A write that needs an unavailable base returns
`MissingLocalVersion` before stage creation or settings replacement.

Confirmation retains the exact staged/read-back record in the existing atomic
state write before removing the stage. Adoption supplies a verified raw record
from the selected inventory. Bootstrap accepts only a complete matching record
with the old key, lineage and space; it does not rename current or dirty settings
as the old version. The bounded History list is not a retention guarantee.

The ordinary publishing worker materializes retained entries as private immutable
`retained-bases/<sha256>.json` files. Only after checked file and directory
barriers does it compact ordinary embedded entries to the current base. Entries
with unknown members are kept. Automatic checkpointing is allowed only on a worker while idle, with no
pending state image. Production automatic transitions currently run on the GUI
thread and defer this checkpoint. Their bounded embedded entries remain intact
until a serialized worker executor can materialize and compact them. Its next exact expected state is decorated before the auto
record is saved; execution still compares the complete canonical bytes. No field
is excluded from a journal hash. There is no object garbage collection in T5a.

## Carry journal and immutable images

One worker owns the ordinary account `QLockFile` and the cached local state.
Ordinary open refuses `CarryPending` whenever `carry.json` exists or cannot be
safely identified, before it can retire pending files. Carry open uses that same
lock and reads state without ordinary stage cleanup. There is no public
skip-validation flag. Recovery journals continue to hold admission.

The private `carry.json` wrapper is bounded to 4194304 bytes and has required
fields `version` (1), `account` (canonical environment/account key),
`binding_sha256` (SHA-256 of the exact account binding token bytes),
`login_authority` (the persistent 64-hex login binding also used by the account's
sync-chat cache), `operation`, `journal_sha256`, `journal` and `objects`.
`journal` is the shared core journal object; its canonical bytes determine
`journal_sha256`. `operation` must equal the core operation id. `objects` maps
64-hex byte hashes to one of `state`, `record` or `text`. Unknown wrapper and core
members survive rewrites. Future wrapper/core versions refuse, without fallback,
deletion or an attempt to resume under a new login.

Images live under `carry-objects/<kind>-<sha256>.json`. State images are bounded
by 4 MiB; record and retained-text images by 256 KiB each. At most 1024 images and
64 MiB aggregate bytes are admitted per journal. A hash collision with different
bytes/kind refuses. Files are private regular files opened without following
links, checked for ownership, permissions, link count, expected bytes and parent
identity. Only fixed kinds and lowercase hashes form paths.

Old, reserved and next state images are stored before the journal can refer to
them. Exact base records remain in embedded retention or immutable carry objects;
preparation refuses if the new state's base cannot be recovered from either.
The adapter does not decorate a core-prepared carry state after its hash has been
chosen. Stage reservation, confirmation and move use the exact core image.

Only the adapter can supply successful `objectsDurable`, `StateObserved`,
`OwnStagePersisted` and `StageRetired` facts. Plan/pending preparation stores the
required images first. Destination verification uses `PrepareSyncCarryCommit`;
cache receipts require the matching account, persistent login, operation and
mark, then actual barriers on the private prepared or admitted cache file.
Cache receipt verification is read-only: the later cache writer must merge
consent and sticky warnings, never replace discovery's accepted baseline.

A replacement checks captured identity, parent identity and exact old bytes,
writes a private temporary, flushes it, rechecks immediately before atomic rename,
and checks file and directory barriers before reporting success. This is a
cooperative account-lock compare-and-swap, not a claim of atomic protection from
an unrelated malicious process. Matching bytes after a failed barrier are not a
durability receipt. Restart re-establishes barriers before asking the core to
resume. Rename before journal acknowledgement therefore preserves the saved
sequence and the core's read-before-resend action. Stage retirement verifies the
matching committed state image, removes only the exact authorized stage, and
checks its directory barrier. Other stages hold recovery.

On POSIX the worker checks `fsync`, and macOS also checks `F_FULLFSYNC` on writable
file handles. Windows checks `FlushFileBuffers` on handles opened with write
access and never flushes a read-only directory handle. The conservative Windows
directory barrier needs a writable volume handle; insufficient rights refuse
`DurabilityFailed`. A supported non-admin directory-durability protocol remains
a platform follow-up. T5a does not claim Windows or physical power-loss
qualification from macOS host tests.

Logout revokes the later executor's RAM authority and closes its worker. The
journal is outside the admitted cache and is retained; a different login refuses
before effects. No logout veto, blocking GUI wait or automatic retry is added.
Terminal journals are retained too. A later executor must explicitly finalize or
abandon them using the approved shared lifecycle rules; T5a does not silently
remove them to make ordinary sync proceed.

## Threads and inherited writes

New carry and durability operations refuse on the application main thread.
Publisher preparation, stage/confirmation and base checkpointing run on the
landed publish worker. Automatic `AutoSession::Event` calls `Transition` directly
on the GUI thread (`purple_sync_auto_service.cpp:249`, `:665-667`); session-ready
work also uses `crl::on_main` (`:630`). This inherited path retains the bounded
embedded records but skips the new worker-only checkpoint. The checkpoint API
still returns `WrongThread` on the application thread. A serialized automatic
worker executor is deferred to its executor slice.
Manual apply still inherits synchronous GUI calls from
`purple_sync_setup_box.cpp:339`, `:553` and `:759` into `ApplySyncConfigChoice`.
The existing state writes in `purple_sync_local_store.cpp:997` (stage),
`:1090` (confirmation) and `:1137` (adoption) now include
the bounded retained records. These inherited calls can carry a larger payload;
T5a adds no separate synchronous pin barrier or main-thread fsync. The exact
post-change write-site lines and validation receipts are recorded in result.md.
Moving manual apply fully to a worker is a separate follow-up.

## Qualification boundary

`purple/test_sync_chat_carry_store.py` compiles the production storage adapters
against the existing Debug objects, without an app build or relink. Its private
synthetic file cases cover retention formats and bounds, unknown preservation,
legacy bootstrap, dirty settings, lock/admission fences, checked barriers,
replacement races, carry commit/stage retirement and restart at rename boundaries.
The existing local-store, automatic, publish-flow and full Purple host gates
qualify ordinary sync behavior. No test makes Telegram calls, changes a native
profile or demonstrates a physical power-loss guarantee.

Automatic regression cases run on the `QCoreApplication` main thread used by the
production service and again with `--worker`. Main-thread checkpoint refusal
must leave state bytes unchanged and create no immutable-base directory, while
Enable and the other ordinary transitions continue normally. The worker variant
also verifies compaction before retirement.

# Per-chat Keep Media

The chat-menu request refers to local cache retention, not automatic media
download. Android already has per-chat retention exceptions. Desktop currently
sets one time limit for both account cache databases in Local Storage settings,
so its chat menu cannot yet show a distinct per-chat rule.

Desktop cache records contain a media key, category tag, size and access time,
but no chat identifier. A document key can be used in more than one chat, and
some cache writes happen outside a chat's download action. Files explicitly
saved to a user-selected path are outside these cache databases.

The desktop implementation should persist a per-chat time limit with an
inherit-global choice, and display the effective limit and whether it is an
override in the History and chat-list context menus. The menu should open the
same per-chat editor from either location. The global Storage setting remains
the fallback. A global-only shortcut labeled as a chat setting would imply
retention that the cache cannot enforce.

Per-chat overrides are appended to the existing encrypted session-settings
payload. Older clients can read the preceding fields, but an older client that
rewrites the payload will omit the unknown tail and discard the overrides.

The storage library can now accept key-specific time limits and apply them to
both stale selection and the next prune timer. It still has no chat ownership
data, so the per-chat overrides are not yet passed to that library and cannot
affect cached files. The storage-library change awaits a desktop compile check.

An encrypted account sidecar now records observed normal-cache keys for direct
message photos and documents. It is bounded, loads before the caches open, and
marks an unfinished write as uncertain after a crash. Message edits and some
deletion paths update the observations. The sidecar does not change cache
retention yet: loaded history cannot prove that a shared key has no other chat
owner, and unloaded date, topic, and sublist deletions are not fully covered.
Streaming, large-file cache, webpage media, and stories are also outside this
first slice. An older client may remove the sidecar during its file cleanup;
the next version then treats ownership as unknown without changing pruning.

For enforcement, associate each cached key with every chat that references it
and both cache database kinds. A shared key should expire according to the
longest applicable time limit among its owners; an unknown owner uses the
global limit only when no chat owner is known. A separate non-chat reference
uses the global limit when that reference can be identified explicitly; a
cache write alone does not establish one. `Never` is the longest limit. The
existing size cap may still evict any cached entry. Externally saved files
must not be affected. Existing cache entries have no recoverable chat owner
and must keep the global rule until ownership is observed. Removing a chat or
message should remove its ownership without removing an entry still used
elsewhere.

Implementation must change both stale-entry selection and the delayed prune
schedule in `storage_cache_database_object.cpp`. Merely skipping entries in
the removal loop would leave the timer running against the global deadline.
Record ownership when messages associate their media keys, not solely from
`FileOriginMessage`: downloads, document/photo caching, streaming and preload
paths can write the same key. Persist ownership before a cache opens so an
older global deadline cannot prune a new per-chat exception on startup.

Verification needs old-cache migration, restart persistence, two chats sharing
one key with different limits (including `Never`), owner removal, access-time
refresh, both cache databases, size-cap eviction, and unchanged externally
saved files. The menu test must show the effective value for an inherited
setting and after an override is changed or removed.

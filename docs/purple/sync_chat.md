# Desktop sync-chat discovery

The Desktop M2B4 adapter uses the shared sync-chat ownership, eligibility and
resolution contracts. Source preparation and unit tests do not establish native
search coverage or authorize Telegram requests, setup or posting.

## Native facts

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

## Source and qualification boundary

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

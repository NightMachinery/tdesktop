# Media auto-reply

The account-specific Media auto-reply screen uses stock `GenericBox`,
`SingleChoiceBox`, `Checkbox`, `InputField`, settings buttons and the native
`ChooseRecipientBoxController` picker. Voice and round video notes are eligible;
ordinary videos, music and text are excluded. Reply text starts as
`@TranscribeThisAI`, with both media Off. A mention does not grant private-chat
access to a bot.

Private chats, groups and channels each have an independent voice four-mode
setting. Round notes follow the resolved voice setting initially. Unchecking
Follow voice settings unlocks their separate four-mode selector. Named sets
contain typed chat identities; the first matching set claims a chat, and omitted
values inherit its chat-type defaults. They never change Work Mode membership.
An account that has no policy does not inherit another account's opt-in.

The parser, policy, action identity, nested lossless writer and durable journal
codec are shared with Android through purple-core. Startup/settings changes
establish activation cutoffs and revision fences. Exact account/session identity
and settings are rechecked before deferred dispatch. History, differences,
edits, unsent media, scheduled and business shortcut items are excluded. A
strict timestamp cutoff also excludes messages in the activation second.

The service bounds pending callbacks at 32, in-flight requests at 8 and durable
reservations at 4096. It holds an account-local journal lock and commits the
reservation with QSaveFile before submitting the native reply. Corrupt, full or
unwritable journals refuse execution. The exact original peer/message/topic is
preserved, including channel posts; there is no linked-discussion remapping.
No optimistic local bubble is created, and ordinary update reconciliation
handles the server response. All outcomes remain reserved, including duplicate
in-flight errors, uncertainty and native permission/flood/paid refusal. No Stars
permission or paid flood-skip flag is supplied and no new-ID retry occurs.

[Telegram's documented contract](https://core.telegram.org/api/updates) provides
same-peer/current-account random-ID deduplication across sessions without
expiry. The feature namespace plus original server message ID is injective
among this feature within the exact outgoing peer. It does not reserve an ID
space against unrelated random-ID users. No live protocol probe is implied by
host qualification.

The only upstream edits are an include and new-message/ACK calls in
`api/api_updates.cpp`. All decisions and dispatch code live in Purple files;
source registration stays in `Telegram/cmake/purple.cmake`. The media reply
splice source joins its source-specific toml++ include list so native and host
recipe builds use the vendored parser header. Native UI rendering,
product builds and live acceptance are separate qualification stages.

Settings or account replacement cancels only request tokens owned by this
feature on their frozen native connection/session. Cancellation is best effort:
a reply the server already accepted may still arrive. Cancelled or uncertain
actions retain their durable reservation and never become automatically
sendable again. Late success/error callbacks do not reopen a reservation or
retire a newer generation's capacity.

Host qualification includes literal production activation and completion bodies
over inert session ports with the actual shared policy and journal. This checks
owned cancellation, late outcomes, account replacement and terminal reservations;
it does not construct a native session or exercise live transport. The source-only
core helper compiles newly added core units with the existing core recipe when
the reused build predates those units, retaining dependency and stale checks.

Automatic requests select the native Sender handleAllErrors policy, so negative,
server, flood and permission failures reach the feature’s terminal completion
without entering default unbounded retries. The native builder and error-policy
bodies are included in inert qualification, alongside literal production
persistence with real Qt file/lock operations and the shared policy.

The native status button shows this account’s last confirmed reservation count,
in-flight capacity and a bounded refusal/cancellation/uncertain reason. A stock
status box explains payment approval without authorizing spending, retry or
journal reset. Reopening the page refreshes the snapshot. Independent round
values that are absent remain labeled as inheritance; chat-type and member
effective values explain mixed sets, and stock radio selection matches the row.

A storage refusal latches the executor before deferred admission and at the
persistence boundary. Already queued actions cannot write or send after that
latch, or replace its original reason with a later local permission refusal.
The queued two-action control uses literal production admission/persistence
bodies, real Qt storage and an inert main queue and transport counter.

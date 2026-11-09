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
source registration stays in `Telegram/cmake/purple.cmake`. Native UI rendering,
product builds and live acceptance are separate qualification stages.

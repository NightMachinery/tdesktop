# Account-backed Purple sync proposal

Status: design plus foundational core. The existing manual Send/Import actions
remain the current behavior.

The shared core now has tested config version construction, remote-head
classification, strict JSON canonicalization, validated uncompressed record
envelopes, config payload inspection and construction, and a versioned
device-local sync state. The shared builder accepts exact UTF-8 settings bytes,
full parent versions, writer metadata, and a sequence, then emits a canonical
record only after parsing and inspecting its own output. A newer settings
schema remains read-only in an older client.

The state records each stream's issued, pending, and confirmed sequence, its
last payload hash, and the config version ancestry. Its read-back checks only
confirm an exact sequence and hash match; a higher own sequence or a matching
sequence with a different hash signals a clone or rewind. Neither client
exposes account-backed sync yet. Desktop-local storage can acquire an exclusive
`sync/lock`, reject malformed or newer state, and stage canonical config bytes
before committing a reserved `sync/state.json`. It creates no state while sync
is off, keeps `sync/` and its pending files owner-only on Unix, and pauses when
a pending file is absent or disagrees with the state. `QSaveFile` protects final
files from partial writes after a process crash; it does not promise persistence
through power loss. The future transport engine must reconcile the install's
own remote record for clone or rewind signals before any publish. Network
transport and the account-sync interface remain to be built.

Compressed library records remain unsupported until the playlist phase.
A deterministic shared-core simulation now exercises three devices against a
fake Saved Messages store, including delayed search, failed upload, edit and
repost modes, conflict classification, and restart from staged publish bytes.

Purple can use each account's Saved Messages to carry configuration and future
playlists between that account's devices. This needs no extra service or
credentials. Saved Messages is a Telegram cloud chat, not an end-to-end
encrypted store (see Telegram's
[FAQ](https://telegram.org/faq#q-so-how-do-you-encrypt-data)). The sync screen
must explain that settings and playlist metadata become available to Telegram's
cloud and to every session signed in to the account. Device-local `state.toml`
stays local.

## Record transport

Each install receives a random 128-bit identity and chooses one home account.
Each platform supplies 16 secure random bytes; shared core formats the same
lowercase base32 install and sync-space IDs on both platforms. An install
publishes its own versioned JSON document for each stream: `config` for
`settings.toml`, and later `library` for playlists. No other install edits that
record. Readers discover records in Saved Messages, read known message IDs again
after reconnect, validate their contents, and reconcile them locally. This
one-writer rule avoids losing updates through Telegram's message-edit API, which
offers no compare-and-swap.

The payload hash uses [RFC 8785 JSON canonicalization](https://www.rfc-editor.org/rfc/rfc8785.html).
Version 1 records reject duplicate object keys and invalid Unicode. Every JSON
number must be an exact safe integer; large Telegram identifiers are strings.
Unknown fields must follow the same rule. This gives both clients the same hash
even if their JSON writers order properties differently.

The preferred transport replaces each install's document in place, keeping Saved
Messages uncluttered. Telegram's
[edit API](https://core.telegram.org/method/messages.editMessage) accepts
replacement media, and source inspection suggests that both clients can edit
document media in the self chat, but this has not been verified between clients.
Before implementation, use a disposable account to test replacement in both
directions, after reconnect, and after repeated edits. If it fails, publish a
replacement message and retire the install's superseded record. The record
format and sync UI can be the same in either mode.

## Settings first

Start with opt-in, per-device sync in Ask mode. App-made settings edits publish
automatically; edits detected from an external editor show a Publish action. A
receiving device retains a visible pending update until the user resolves or
snoozes it. Each whole-file version records its ancestry, so a device can
distinguish a newer version from concurrent edits. Concurrent versions require
an explicit choice, and the discarded side remains in local History with Undo.
There is no automatic TOML merge in this first release.

Before this new flow, repair the current manual sharing path: prevent self-echo
offers, mark a send complete only after server confirmation, align import limits
between clients, and preserve Android's pre-import backup across editor saves.
Update the existing documentation to describe Saved Messages' cloud privacy
accurately.

Android source `6df36b1d` now keeps a separate pre-import backup across editor
saves, and `a950e13c` matches desktop's 4 MiB import and 100-document search
limits. That signed build opened the retained test account on 2026-09-29; the
backup and large-file boundaries have not yet been exercised in a live import.
Both legacy sharing paths now wait for a confirmed server message id before
recording the sent fingerprint and advancing the account's offer watermark.
An older send that finishes after a newer edit cannot replace the current
file's sent fingerprint. The Android receipt patch built from exact source
`8b07614c`, passed signature checks, and upgraded the preserved emulator
without losing its login; its signed APK was delivered privately. Neither
client has had a live receipt test; compile and startup checks do not prove
server acceptance handling.

On desktop, `UploadTo` returns after `sendFiles` queues file preparation. A
single-file receipt follows the post through preparation and upload to the
`sendPreparedMessage` server response. Only a response with this post's server
message id records the fingerprint and advances the import-offer watermark.
Preparation, upload, and request failures leave both unchanged. A fingerprint
pending in memory prevents a second local write from queuing duplicate bytes
while the first is in flight. Telegram's
[update protocol](https://core.telegram.org/api/updates) says `random_id`
deduplicates sends across an account's sessions and `updateMessageID` can later
identify a message whose response was lost. The current legacy path does not
reconcile a lost response after restart. A later explicit save or manual send
may create a duplicate if the server accepted the first post; durable
`random_id` reconciliation belongs in the account-backed design. This pipeline
has not yet been tested live.

On Android, a Purple-only document receipt follows the asynchronous send
helper to the server result. Each attempt stages an immutable file with the
required `settings.toml` name, and concurrent sends of identical bytes on one
account share the pending attempt. Telegram's own Retry action after a failed
send does not preserve this Purple receipt, so a successful manual retry can
still cause a later duplicate or self-offer. The new sync engine needs durable
reconciliation rather than extending this legacy callback indefinitely.

## Playlists later

Playlists will use the same account binding, record envelope, discovery, and
transport. Their data model should be revisited with the playlist feature.
Unlike a whole settings file, individual playlist entries can be edited
frequently on different devices, so the current proposal uses a mergeable
library state with stable song references, per-field conflict detection,
ordering, and deletion markers. It stores message and document identities, not
audio files or account-specific access hashes. Unavailable songs remain visible
with an explanation instead of disappearing.

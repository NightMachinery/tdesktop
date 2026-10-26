# Sync-chat setup on Desktop

The per-account Sync chat row opens a status and setup box. Its snapshot is tied
to the opening account, login, root, Work Mode, app-wide source, binding and
resolution run. Every callback and confirmation checks that authority. Background
resolution never opens a dialog.

A complete Unset result, an exact empty inventory, and no settings binding,
effective cache or carry are required for fresh setup. Existing and bound
streams remain read-only. The fresh chooser offers a private Purple channel,
Saved Messages and up to fifty currently loaded chats, including archived ones.
Full info is read before a row is enabled and again before acceptance. One
DesktopSyncChatSetupEligibility predicate delegates to the same core eligibility
used for mark acceptance and posting. Unknown rights, admins or readers refuse.
The confirmation rechecks the disclosed reader snapshot, including available
member digests. Changes require a fresh confirmation. No account roster is synced.

Reader disclosure uses account-settings r3 for every account: settings can name
chats and people from other accounts added to Work Mode lists. All readers can see Telegram cloud sync data,
which is not end-to-end encrypted, playlist sources and the Purple mark. The C6
persistent warning is shown without blocking posting under prior consent. The
current reader counts, pre-history distinction and unavailable-list blind spot
are stated. Refused marks name the chat and the core refusal reason.

Setup confirmation currently ends in a qualification-held state. It cannot call
a write executor or Telegram. This source/unit delivery does not authorize
channel creation, marks, pin, mute, archive, migration or automatic non-Self sync.
The normal Self-only automatic guard remains in place. Native outward execution
requires a separately reviewed transport and the user's explicit GO.

## D-M2B4-8

An exact zero-row response is Unknown. Peerless messageEmpty entries in history
or inventory are Unknown and hold the inventory. Only an R3 reread of the cached
mark may classify a same-id peerless messageEmpty as MarkGone, with independently
readable chat evidence. Wrong ids, contradictory peers and unreadable chats are
Unknown. Record exact reads require an explicit matching peer on messageEmpty;
peerless and zero-row record replies are Unknown, including legacy Self. The
cached-mark R3 exception does not apply to records. The model now checks zero rows defensively too; the service already
refuses them. The genuine TL fixture exercises these boundaries across Self,
ordinary chats and channel routes. Native N3 must still record whether Telegram
supplies peer_id for a deleted Self or channel mark.

All implementation and registration is in Purple-owned paths; no upstream hook
or Copy-as-Markdown path is changed. The existing Sync setup box only adds a
per-account Sync chat row and an opening-account fenced child box.

## Settings disclosure in this build

This Sync chat dialog is opened only from the per-account settings Sync screen;
there is no playlist-only setup entry point yet. Both existing-chat and Create
confirmations therefore show the settings reader warning and amendment-13
cloud disclosure for every account, including when setup is still held or
settings posting is off. The text is not conditioned on settings ownership.

Actual automatic settings sync is on when the account's private SyncAutoRecord
has enabled=true (AutomaticSettingsSyncEnabled). A modern per-account settings
store also supports explicit manual settings operations; its initialized binding
is not an automatic-sync switch. The legacy file-local sync.sendAfterSave option
belongs to the legacy send-after-save route. None of those switches hides this
setup disclosure, so an enabled account can never miss it. The confirmation names
the chosen chat and states that state.toml stays on this device.

## Offline fresh-setup executor

The Purple-owned executor exposes Next and Complete, with injected transports and
a durable save callback. It has no production write transport and no UI callsite.
Genuine TL builders prepare private broadcast creation, a silent canonical mark,
a silent pin, origin-account mute_until INT_MAX and folder 1 archive requests.
Tests inspect those requests; they never send them. Limit, restricted and flood
responses remain typed. Try again is explicit, resolves again, has a three-create
budget and honors a persisted flood deadline. Saved Messages fallback is allowed
after a definite rejection; an unresolved ambiguous creation stays held.

The journal is per account, private and worker-only. It takes the same account
lock as other modern sync writers. Admission requires a domain-separated digest
of the persisted login authority, account, root, Work Mode, home account and
binding. RAM origin and request tickets separately fence each callback. A later
native resolver must borrow this account lock through DesktopSyncChatOperation;
it must never independently take a second writer lock. Journal fields are
mandatory, typed and canonical, including the operation UUID and stable mark
random id. Missing, extra, duplicate, newer or malformed data refuses without
overwriting or deleting it. An existing operation cannot be replaced by a new
one. A changed login or context refuses the old journal.

Every write intent is saved before a command is issued. After a crash, an
uncertain createChannel only emits ReconcileCreate. A title match, a lone loaded
channel, a search absence or an RPC timeout cannot justify another creation.
Only retained Updates tied to the exact operation's create request can identify
its result; without that evidence reconciliation holds. The complete Updates
receipt is saved before ApplyUpdates; the latter must finish normal upstream
Updates handling before PeerInfo or a mark can run. Unknown eligibility, admin
counts or readers refuses. A separate complete exact inventory of the chosen
target must be Empty before the first mark, including on a not-yet-issued
post's explicit resume. Present or Unknown is held. A second fresh full-info and
reader check follows that inventory before the unissued mark. The actual typed
scanner, inventory finisher and production inventory-reply mapper preserve that
evidence. A changed initial reader disclosure holds with a fresh consent offer.
Only an explicit acceptance for the same account, operation, ticket and unchanged
destination resumes it, and that resume repeats resolution, inventory and full
info. Aborting or refusing the offer writes no consent. Pending mark intent is
never converted into an unissued post by reconsent.

A resumed Pin, Mute or Archive with a sticky new-reader warning first issues
an explicit cache Commit, retaining its exact decoration resume point in the
journal. A failed cache write holds. The original baseline and warning survive
subsequent departures and another restart.

The mandatory journal contains the original reader snapshot as well as its
digest. After a mark has been issued, known new readers retain prior consent and
set a sticky C6 warning. Departures alone do not set it; unknown readers and lost
rights still hold. The cache uses the original accepted counts and member
digests across restarts, so the new population cannot silently become the
accepted baseline. The snapshot and warning are local only, never synced as a
roster.

The mark random id is saved before sending. Missing or ambiguous acknowledgements
reconcile that same random id. The whole exact TL readback response is retained;
inexact, flood-limited, zero-row, duplicate-row and contradictory-count responses
hold. A valid response then requires an exact canonical owned text, id,
date, typed peer and no media. Cache commit follows that reread. Only a newly
created channel then advances through silent pin, mute and archive. Existing
chats and Saved Messages finish after the mark/cache step and are never pinned,
muted or archived by setup. These steps are idempotent and are recorded before
and after each effect. Restart/resume first resolves again and revalidates full
info and an already known mark. A bound stream, Follow/Fill/carry or an uncertain
inventory stays held. A newly resolved stream may be adopted before any own
created/marked work; it is never overwritten.

One worker owns all executor methods and the journal store. The foreground owner
revokes an atomic capability immediately and queues cancellation to that worker;
no GUI thread enters file IO or waits for an executor callback.
The foreground owner supplies an atomic current capability revoked on logout,
login reuse, source or binding change, account switch and close. The GUI never
waits for this executor or file adapter. Cancel drops the in-flight ticket,
keeps uncertain evidence and starts no cleanup. A stale completion cannot cancel
a newer ticket. Store failure visibly holds and preserves the last durable
checkpoint. No step runs on a background discovery timer. Native UI limit boxes,
normal Updates integration and outward execution remain behind the separate GO.

Android A3 shares these states, journal/ambiguity rules, request flags and reader
consent boundaries. Platform details differ: Desktop uses a QLockFile/private
atomic-file adapter and cancelable MTP::Sender full-info reader; Android uses its
account-specific storage and request owner. D6/D7/C6 policy already landed on
Desktop; Android delivers its alignment separately. Neither source delivery is
native or outward qualification.

During this qualification hold, Desktop keeps a confirmed choice only in the
open box and asks again after reopening. Android may retain a device-local
intent receipt. This conservative Desktop difference grants no posting consent
in the live discovery cache and changes no destination. The durable creation
journal is exercised only by injected tests until outward GO; activation must
load or resume it before creating a new operation.

The exact setup mark uses the shared canonical settings/playlists and deletion
explanation, followed by #purplechatsync v1. Discovery's grammar is unchanged.

## Input lifetime

PeerData::input(), ChannelData::inputChannel() and UserData::inputUser() return
owning TL values. Retain that value in a named local before keeping a reference
to c_*() or data(). A reference returned by an accessor on a temporary does not
extend the temporary's lifetime. Both full-info and cached-mark channel reads
retain the input while copying the channel id and access hash into their request.
The existing purple/test_sync_chat_setup_routes.py source gate scans all Purple
C++ sources and tests unsafe and safe controls. Reaching these exact session-owned
request paths in a standalone unit would require the native account/data stack.

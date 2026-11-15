# Playlists

The Desktop build includes the shared playlist foundation from purple-core:
strict library and local-state codecs, edits and undo, deterministic merge,
bounded gzip records, record discovery, verified cover checks and complete
sync-step planning. Its contract and focused tests are described in
[the core playlist documentation](../../Telegram/ThirdParty/purple_core/docs/playlists.md).

The Desktop candidate adds account-local playlist menus and basic saved-order
playback. It is a private, uncompiled integration draft. Advanced queue modes,
downloads and sync transport remain to be implemented.
Sharing is deferred, and changing the sync chat does not clean records from the
old chat.

The gzip codec uses the existing `desktop-app::external_zlib` dependency.
Playlist sources are registered in `Telegram/cmake/purple.cmake`, alongside
the other shared Purple sources.

Private music listening is connected independently of the playlist UI. It is on
by default for all music playback, with an app-wide switch under Settings >
Purple > Media playback. See [media playback](media_playback.md).

## Account-local storage

`DesktopPlaylistStore` stores `library.json` and `local.json` under the account's
`playlists` directory from `ResolveAccountFiles`. It accepts a captured account
routing snapshot and a mandatory, thread-safe route validator. Callers must
revoke that validator when the account closes or the route changes. Opening
also requires a readable, valid root-owner record matching the captured owner.
The store then checks directory identities and the captured local-choice and
owner files throughout writes.

One worker thread owns each store, and a per-account lock prevents competing
writers. Reads return a revision and exact file snapshots. A stale revision,
changed file, changed route, symlink, unsafe permissions or reentrant operation
refuses the write. Missing files yield the core defaults; unreadable, invalid
and newer files remain untouched and block writes. Local reads use the observed
file size rather than a fixed record-size cap.

Writes use the shared durable atomic writer with exact readback and file and
directory barriers. A changing merge saves the previous library bytes as
`library.json.bak` first. The new library must be confirmed before its local
state or memo is saved. A first memo also requires a materialized library,
including an empty one. A failed state write can leave a confirmed new library
with the previous memo, so the next read safely retries unfinished work.
Results distinguish confirmed writes from files that may have changed before a
durability failure; callers must reload after any attempted write.

Callers derive edits from the returned typed state, preserving its unknown
fields, and serialize local edits and merge results through the same store.
The storage component performs no listing, upload, pruning or account login.
`cases-playlists-store` exercises real temporary files, the real codecs and
durability helpers, with controlled routing changes and write failures.

## Account service

`ReadAccountPlaylists` and `EditAccountPlaylists` lazily create an account-owned
`crl::object_on_thread` worker. Reads and edits run serially on that thread,
including opening and releasing the per-account store lock. Each edit reloads
the current library before applying a typed shared-core operation, preserves
unknown local fields and the earliest unsynced age, then reloads after an
attempted write. Results keep core edit errors and partial durable-write
outcomes separate from the current snapshot.

Only the main thread captures the account routing and touches session objects.
Routing changes, ordinary or forced logout, and session destruction revoke an
atomic token before retiring the worker. Queued work checks that token through
the store's durable-write guards. Already committed files remain committed;
revocation cannot undo accepted writes. Main-thread completions recheck the
route, and destroyed sessions receive no late callback. A retiring worker may
briefly retain its lock, so a new route can report Busy until it has closed.

The service performs no automatic account-file access at startup. The playlist
UI will request a read when needed. `cases-playlists-service` exercises the
production service body with controlled account and routing ports, real CRL
threads, temporary files, shared-core edits and the durable store.

## Song resolution

`DesktopPlaylistResolver` reads sources for the captured session and account.
It checks loaded messages first, then batches up to 100 sources through the
shared `SyncRequestSender`. Self, user and basic-group references use
`messages.getMessages`; channels use `channels.getMessages` and their access
hash. An unknown channel can first use its saved public username hint, but the
resolved numeric channel must match the stored channel. A reassigned username
cannot redirect a song. Missing hints or access report No access; callers can
retry when the channel becomes known on that device.

Every successful source must be the expected document, a regular server song
in the message's main media, and permitted by the existing save policy.
Definitive Deleted, No access, Changed or Restricted results try the saved
fallbacks in order. Network failures stop that row until the next explicit
resolution; flood waits remain with the shared request scheduler. Resolving a
fallback never relinks the stored song. Invalid or incomplete response shapes
cannot establish that a song was deleted.

The resolution queue preserves row order, yields between chunks even for
cached rows, and drops late or duplicate completions. It returns source
references for playback through the stock message player and file-reference
refresh path. Results are memory-only. `cases-playlists-resolver` exercises the
production queue and channel-identity check with controlled cache/read/defer
ports; the real Telegram adapter is syntax-compiled. Live RPC behavior and
playlist UI/player integration are not yet qualified.


## Desktop local playlist interface

The main menu has a Playlists entry controlled by the app-wide, default-on
`playlists.menu_item_p` setting. The music-message Add to playlist action remains
available when that entry is hidden. Both surfaces bind their account and route
when opened, and reject stale source messages and late callbacks.

Purple-owned boxes reuse stock settings buttons, single-line inputs, labels and
box spacing. The UI calls the global `::Settings` helpers and uses the stock
`lng_settings_save` phrase and `window.style` main-menu button.
The library, recently deleted view and song detail use pages of at
most 25 entries. Create, rename, copy, delete, restore, remove and structured Undo
submit typed account-worker edits and display the confirmed reloaded snapshot.
Songs retain saved title, performer and duration without requiring their source
to resolve. Display order comes from the shared core, not the song map's order.
Drag reorder and a virtual song list remain later work.

`RecordAccountPlaylistUse` is a separate typed worker request. Open, play and add
update local recent-use order without changing sync bookkeeping. Its guarded
`writeLocalState` path preserves library and backup bytes and absence exactly,
including valid noncanonical library files. Library edits still use the existing
library-before-local-state durability ordering. Failed or partially durable
writes remain errors even when the reloaded snapshot contains part of a change.

The UI delegates Play, song selection, Retry and playback status to the separate
`purple_playlist_player` adapter. UI lifetime does not own playback. The adapter
owns source resolution and stock player navigation; unsupported modes are shown
as problems. This interface requires the player adapter to be integrated along
with the UI. Full queue modes, retained downloads, sync transport and sharing are
unfinished. The sync line says local-only rather than claiming a sync succeeded.

The shared settings scope registry makes `menu_item_p`, `keep_downloaded_p` and
private listening app-wide. Automatic playlist sync is per account; its local
sync switch is per account and device. Future downloads use per-playlist
On/Off/Inherit plus the global default and Remove downloads now. Changing sync
chat will publish a snapshot without removing old-chat records. Sharing is for a
later release.

## Basic saved-order playback

The basic player adapter snapshots one account's saved order and sources in
memory, then resolves the selected row before starting the stock message
player. It resolves at most three following rows while that song plays and
retains at most four cached resolutions. Unavailable metadata rows stay in
the snapshot. Definitive failures skip in the requested direction; a failed
resolution or playback admission asks for explicit Retry. Shared resolver
flood-wait scheduling remains unchanged. Playing a fallback never relinks or
writes the saved library.

Next, Previous and automatic end continuation are handled before the stock
chat queue. Exhaustion stays a handled boundary. Voice playback, Repeat One
and the stock disable-autoplay option retain their own behavior. Shuffle,
reverse and Repeat All are refused with a typed status in this first slice;
they are not implemented playlist modes. Queue panel contents, virtual drag,
download management, sync and sharing remain follow-ups.

The status producer and start/retry/stop API capture the selected session,
account and routing authority. UI/controller destruction does not own ongoing
playback. Service route invalidation, account retirement, session destruction,
player shutdown and unrelated song takeover invalidate old callbacks. The
expected AudioMsgId is installed before synchronous stock play callbacks.
The account service invalidates playback before retiring workers and stops the
captured session on logout. A live controller subscribes to data-clear notification. Previously retired
controllers have already removed that subscription; their retained resolvers may
instead drain at session lifetime destruction. Actual document/destructor order
remains unqualified and requires native validation.

The proposed `cases-playlist-player` fixture extracts the literal cursor,
resolution classification, bounded-window and handoff authority helpers. It
covers saved order,
exhaustion, explicit transient retry admission, stale generation, retired and
destroyed authority, and forward/backward windows. Production adapter/player
syntax checks are registered separately. These proposed tests have not been
compiled or executed. They do not prove real RPC, stock streaming, actual
session destruction or synchronous player observer behavior; ROOT must run
those integration and lifetime checks before shipping this draft.

## Synchronous stock-player handoff

A stock player subscriber can call Play while the previous playlist call is
inside `clearStreamed` or `emitUpdate`. The Purple handoff adapter retains only
the newest nested Song request until the native call returns. Narrow guards in
`Instance::play`, `playStreamed`, `clearStreamed`, `setCurrent`, `emitUpdate` and
`stop` reject older continuations after synchronous notifications. This avoids
an older clear or stream assignment overwriting a newer request. It does not
claim that stopping an overwritten stream could restore its successor.

Deferred stock requests capture a weak session and reject logout or session
replacement. Deferred playlist requests retain numeric resolution data and
revalidate the source before playback. New playlist intent and player shutdown
revoke pending work. Voice calls bypass this Song-only handoff authority.
Only actual native compilation and streaming event-order checks can qualify
these seams; the drafted helper assertions do not prove native lifetimes.

## Upstream hooks

The main-menu and message-menu hooks delegate to Purple-owned UI helpers.
`media/player/media_player_instance.cpp` delegates saved-order navigation,
shutdown and synchronous handoff authority to Purple. All policy, deferred
request ownership and playlist state remain in Purple sources. The exact hook
inventory is maintained in [upstream_hooks.md](upstream_hooks.md).

Ordinary stock Song Play and Stop enter shared intent authority before native
work. Stop revokes a pending library read, initial/manual-step resolution and
queued start even when the current song id has not changed. A newer ordinary
Song request during Retired publication invalidates the suspended older playlist
start before it constructs a controller. Adapter-owned Play/Stop and native
Repeat One consume a one-shot internal authorization at the same entry hook.
Synchronous subscribers cannot inherit that already-consumed authorization.
Voice bypasses this Song policy, and the existing autoplay decision order stays
in stock code. These intent corrections remain uncompiled and unexecuted.

## Sleep timer coexistence

The timer consumes a matching ended item before repeat or automatic movement.
A later manual Play advances the owned saved-order playlist once. Exhaustion
stays exhausted; ordinary chat playback retains the timer's existing fallback.
Song callback scopes defer a synchronous successor until native mutation ends,
and a stream delivery token rejects retired callbacks. Guard checks after end
notification and timer publication prevent the old frame from consuming or
clearing the successor. Held seek intent remains item-bound; timer expiry blocks
automatic resume while explicit Play can resume. This composition has only static
review and still requires production compilation and real streaming traces.

## Sleep expiry and pending playback

Before sleep expiry publishes or pauses, Purple cancels the playlist cursor and
resolver admissions, increments the ordinary queued-start generation and drops
queued native handoffs. Late store replies, resolution replies and skip/prefetch
continuations cannot authorize playback. The controller retains its saved songs
and completed cache entries. Pending owned Next returns to the row actually
loaded; an initial pending selection becomes Retry required. Status notification
is deferred and admitted against the new cursor generation.

Expiry does not call native Stop or retire the stream delivery token. Normal
paused-stream Play resumes natively. If a canceled in-flight handoff had already
stopped the outgoing owned stream, explicit Play can resolve its retained row
with fresh admission instead of losing the queue through ordinary external Play.
The end-item manual-advance seam runs first. Explicit Next or Retry after expiry
creates fresh cursor authority; earlier callbacks remain revoked. These joined
expiry and player paths still require native verification.

The first owned resume command after expiry is admitted against a fresh cursor
and waits for active Song frames to unwind before invoking the existing native
resume. This lets explicit Play from an expiry notification survive the remaining
expiry pauses. A playPause action that requests Pause keeps the stock pause path.
No stopped-row resolution is used for an ordinary paused-stream resume.

A user Pause cancels an admitted but undelivered expiry resume before checking
its native frame, and obtains a fresh pause frame if that queued resume revoked
the enclosing one. The timer's own pause consumes a one-shot Pause token first;
it does not cancel a manual resume requested after expiry. This distinguishes
actual pause intent without changing the existing Play/Stop tokens.

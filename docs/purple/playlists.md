# Playlists

The Desktop build includes the shared playlist foundation from purple-core:
strict library and local-state codecs, edits and undo, deterministic merge,
bounded gzip records, record discovery, verified cover checks and complete
sync-step planning. Its contract and focused tests are described in
[the core playlist documentation](../../Telegram/ThirdParty/purple_core/docs/playlists.md).

The Desktop build adds account-local playlist menus and basic saved-order
playback. The integration is production-compiled; complete host and native/GUI
qualification remain pending. The private queue-mode proposal below adds global
order and repeat; queue panels, downloads, sync transport
and Android parity remain to be implemented.
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
The library and recently deleted view use pages of at most 25 entries. The
private virtual-list proposal below replaces pagination in song detail.
Create, rename, copy, delete, restore, remove, reorder and structured Undo submit
typed account-worker edits and display the confirmed reloaded snapshot.
Songs retain saved title, performer and duration without requiring their source
to resolve. Display order comes from the shared core, not the song map's order.
Drag reorder and the virtual list remain uncompiled until separately qualified.

`RecordAccountPlaylistUse` is a separate typed worker request. Open, play and add
update local recent-use order without changing sync bookkeeping. Its guarded
`writeLocalState` path preserves library and backup bytes and absence exactly,
including valid noncanonical library files. Library edits still use the existing
library-before-local-state durability ordering. Failed or partially durable
writes remain errors even when the reloaded snapshot contains part of a change.

The UI delegates Play, song selection, Retry and playback status to the separate
`purple_playlist_player` adapter. UI lifetime does not own playback. The adapter
owns source resolution and stock player navigation, including the existing
global order and repeat controls. This interface requires the player adapter
along with the UI. Queue panels, retained downloads, sync transport and sharing
are unfinished. The sync line says local-only rather than claiming a sync succeeded.

The shared settings scope registry makes `menu_item_p`, `keep_downloaded_p` and
private listening app-wide. Automatic playlist sync is per account; its local
sync switch is per account and device. Future downloads use per-playlist
On/Off/Inherit plus the global default and Remove downloads now. Changing sync
chat will publish a snapshot without removing old-chat records. Sharing is for a
later release.

## Saved-order playback and global modes

The basic player adapter snapshots one account's saved order and sources in
memory, then resolves the selected row before starting the stock message
player. It resolves at most three following rows while that song plays and
retains at most four cached resolutions. Unavailable metadata rows stay in
the snapshot. Definitive failures skip in the requested direction; a failed
resolution or playback admission asks for explicit Retry. Shared resolver
flood-wait scheduling remains unchanged. Playing a fallback never relinks or
writes the saved library.

Next, Previous and automatic end continuation are handled before the stock
chat queue. Global order and repeat come from the existing player controls and
Core::Settings. Normal uses the saved order; Reverse uses its reverse. A song
selection starts that row first in either order. Play without a selected row
starts the first row of the chosen order. Shuffle keeps the selected row first,
or the current song when it belongs to the new snapshot, then randomly orders
all remaining rows once in memory. With neither a selected nor a current row,
its first song is random too. The detail box also offers the stock Shuffle
action and changes the same global order setting.

Repeat None and Repeat One keep non-wrapping manual boundaries. Repeat One
continues through the stock end hook before automatic stepping. Repeat All
wraps Next, Previous and automatic stepping, including a one-song queue. A
complete traversal containing only definitively unavailable songs exhausts
instead of looping. Exhaustion remains handled and never falls into the chat
queue. Voice playback and disable-autoplay retain their stock paths.

Changing global order rebuilds the in-memory traversal around the current
owned song, cancels old resolver admission and clears position-indexed cache.
It does not restart the native song. Shuffle starts at that current song;
normal and reverse retain its position in the original snapshot. A pending
unowned selection is resolved again with fresh admission, except a selection
paused by timer expiry stays Retry required until an explicit command. Changing
order alone cannot undo expiry. A retained row already requiring Retry stays
Retry required; order changes do not silently retry a failed source or stream.
Repeat changes update wrapping without rewriting the library. Snapshot edits take effect only
on the next Play. Status document identity and row index continue to refer to
the original visible snapshot rather than a shuffled traversal position.
Queue panel contents, virtual drag, download management, sync and sharing
remain follow-ups.

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

The `cases-playlist-player` fixture extracts the literal cursor,
resolution classification, bounded-window and handoff authority helpers. It
covers saved order,
exhaustion, explicit transient retry admission, stale generation, retired and
destroyed authority, and forward/backward windows. Production adapter/player
syntax checks are registered separately. The fixture has compiled, but its host
qualification is pending. It does not prove real RPC, stock streaming, actual
session destruction or synchronous player observer behavior; native integration
and lifetime checks remain required before shipping.

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
The production units compile. Streaming event-order checks remain required;
the helper assertions do not prove native lifetimes.

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
in stock code. These intent corrections compile; native execution remains
unqualified.

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

## Queue-mode candidate qualification

The queue-mode extension is a private source proposal, not native qualification.
The existing helper fixture gains deterministic order/permutation checks,
selected-row reverse traversal, wrapped availability, one/empty-song behavior,
all-unavailable cycle termination and unique bounded wrapped lookahead. Old
cursor, handoff, timer and token assertions are retained. The same driver also
syntax-checks the changed stock-box adapter. No new gate is required.

Resolver batches capture their exact traversal indices, including a repeat
boundary, and validate document identity before caching each result. Foreground
resolution still requests the selected row first; at most three later rows are
warmed. Transient failures still require explicit Retry, and sources are never
relinked. Automatic continuation skips rows whose resolution is still pending
and uses the next completed result without waiting for a network operation. If
no completed row remains, it exhausts. Manual selection/Next and explicit Retry
may resolve a row asynchronously, matching the existing foreground API. Cache
contents remain bounded to four traversal positions.

A live order change revalidates the actual owned current message against its
saved primary or fallback source, then keys that completed resolution to the
new traversal position. This does not replay the native stream or request RPC.
It retains Repeat All eligibility for a one-song queue, including a mode change
while expiry-paused followed by ordinary native resume. Missing, changed or
restricted sources do not gain a completed cache entry.

The native shuffle pager is suppressed only after Purple claims a delivered
Song or an intentional owned handoff. Expected-id equality during an unowned
Loading or Retry-required attempt is insufficient. Retired attempts and Voice
leave stock navigation alone. Its existing chat-history window remains; the extra whole-chat shuffle listing
would otherwise run even though Purple never uses it. Ordinary chat shuffle and
Voice retain their stock behavior.

Later validation must compile the changed player, native hook and stock box,
then run the existing fixture and real queue traces across all order/repeat
combinations. Native checks must cover selected-first playback, live mode
changes during held resolutions, actual shuffle randomness and chat-request
suppression, final/empty/unavailable boundaries, Repeat One and autoplay order,
and timer expiry followed by manual advance/resume. Helper assertions do not
prove streaming, global-setting publication or timer coexistence.

The correction fixture tests the actual document-to-traversal mapping helper
across all order modes and the one-song current-slot contract. Those checks do
not exercise controller cache repopulation or the native pager. Native regression
must cover one-song Repeat All through live Normal/Reverse/Shuffle changes and
expiry pause/resume, plus a larger queue's rekeyed current slot. Separately hold
an unowned playlist attempt while a chat song plays, enable Shuffle, retire the
attempt by route change and verify the chat pager/navigation remain intact.
Owned playlist handoff, no active controller and Voice are comparison cases.

## Virtual detail list proposal

The private detail-list proposal replaces song pagination with one painted
content widget under the stock media-player ScrollArea. It paints the clipped
row range and retains metadata rather than a widget per song. The row uses
mediaPlayerFileLayout padding, thumbnail/play glyph, normal and semibold fonts,
stock window colours and the menu reorder glyph. The extra source-state line
uses those font metrics. Library pagination remains unchanged.

Handle drag previews an in-memory insertion, with edge autoscroll. Escape,
focus/window loss, hiding, disabled/pending state and mouse-grab loss cancel
without a write. Arrow/Home/End/Page keys select, Enter/Space plays, and
Alt+Up/Down or the two stock Move buttons submit a reorder. Remove acts on the
selected row. The screen-reader list exposes virtual stable row identities,
selection, rectangles, names and guarded main-thread focus/activation actions.

A drop delegates a complete permutation to the existing typed ReorderSongs
account edit. Pending edits disable interaction; only a confirmed returned
snapshot replaces saved order. The existing confirmed Undo record restores it
with the core conflict checks. Refresh and navigation are serialized with edits.
Account, route, controller, box lifetime and reply generation guards remain.
Playback continues using its immutable queue until another Play. No store,
worker, playback facade or timer API changes are proposed.

ShowPlaylistBox(controller, playlistId, focusedDocumentId) opens and reads the
bound account playlist, then reveals/focuses a matching saved row. Focus never
starts playback. A row removed by newer edits is not reinserted. Existing
ShowPlaylistsBox remains the library entry.

Source state is limited to the public playback status for its reported row;
other rows say Source not checked. There is no background availability scan or
invented downloaded mark. Current means a delivered playlist row whose document
still matches the native Song player, including a paused item. This proposal
adds no downloads, sync, sharing, queue panel or new playback modes.

Native compilation and a real 2,000-row performance/GUI pass remain required:
long and RTL metadata, light/dark comparison with stock player rows, keyboard
and assistive activation, drag across viewport edges, cancellation, concurrent
membership changes, failed/partial writes, Undo conflict, account/route loss,
and opening/focusing rows after refreshed snapshots. Static source checks do
not establish rendering, smooth scrolling or service/streaming behavior.

## Player bar and hover list

An owned playlist Song uses a Purple target query that checks the captured
session, route and actual playback claim without retiring or publishing state.
Pending unowned attempts, retired
controllers, another account and Voice do not claim this UI. The target keeps
the playlist identity and the actual current document, even if another row is
being resolved.

The stock hover panel returns no peer for that target, so it removes the
unrelated chat list. Clicking the player label opens that account's playlist
box and asks its virtual list to reveal the current saved document after a
confirmed read. It does not select a new queue or start playback. The ordinary
Voice and cross-account message actions keep their stock behavior when there
is no owned target. The label uses the stock pointer cursor for this action.

The UI bridge requires the virtual-list box entry point `ShowPlaylistBox`.
Native ownership/publication, account changes during opening, reveal after a
library edit and real hover/click behavior require integration qualification.

The Current marker also uses that owned-current target directly. A newly opened
view therefore marks the delivered song while a successor resolves or after
exhaustion, independently of earlier Playing notifications. The checked-source
status still belongs only to the facade's reported row. Pending unowned attempts
do not gain a marker from document membership. Native checks must compare fresh
and existing views with a held successor, paused/exhausted state and unowned
initial playback.

## Desktop download foundation

Desktop owns download state in the preserved `desktop_playlist_downloads` member
of account `local.json`. Format 1 has `preferences`, keyed by playlist ID with
`"on"` or `"off"` values, and `files`, keyed by canonical decimal document ID.
An absent preference means Inherit. The app-wide `playlists.keep_downloaded_p`
default stays true. A file entry contains a safe lowercase ASCII `extension`,
positive decimal-string `size`, filesystem `identity`, and lowercase hex
`sha256`. Unknown root and entry members survive writes. Malformed or newer
client download state refuses typed download commands; normal playlist edits
and recent use remain usable and preserve that opaque value. Core local-state
and library validation remain mandatory for every operation.

Optional `pending` entries are keyed by a random 32-character lowercase hex
operation token. Each contains `playlist`, decimal-string `document`,
`extension`, decimal-string `size`, `identity`, and `sha256`, plus preserved
unknown members. Empty identity means a confirmed filename reservation before
file creation. Empty digest means the complete file has not been checkpointed.
The worker confirms the reservation, exclusively creates the private staging
file, then confirms its identity before transfer starts. Staging paths are
always derived as `files/.incoming/<token>.part`; final paths are derived as
`files/<document id>.<extension>`. JSON never supplies a path.

An explicit `DesktopPlaylistDownloadJob::start` snapshots visible rows and uses
the real source resolver. At most two rows can resolve or transfer at once.
Each transfer owns a unique `DocumentData::createStreamingLoader` result,
a dedicated streaming `Reader` with no shared cache, and its own
`createStreamedDownloader` result. The downloader's location key is null, so it
cannot register the staging file as the document's location. Neither the
ordinary document loader nor a shared reader is retargeted or cancelled.
Existing local-file and memory-byte sources use this same dedicated path.
Purple's existing song/save qualification governs protected content.

Native completion is deferred beyond the update callback before releasing the
owned downloader and reader. The worker independently verifies expected size,
identity and digest, confirms the completed digest in the intent, publishes
without overwriting an existing destination, and confirms inventory readback.
Only then can the guarded main-thread completion call `setLocation` and
`session.local().writeFileLocation`. An ordinary active document loader postpones
that final registration as Busy; explicit Retry can register the verified kept
file after the ordinary operation ends. Playback alone never enters this path.

A failed final inventory write retains a durable intent naming the owned file.
Retry revalidates the source and recovers verified staged or published bytes,
including an interrupted two-name publication, without adopting an untracked
existing destination. Partial/missing owned stages can be replaced by explicit
Retry. `DiscardTransfer`, Remove downloads now and pruning clean up only proven
owned artifacts; other unpurged references, including Recently deleted, protect
retained files. An identity-less reservation whose file unexpectedly exists
refuses Busy because ownership cannot be proved. It remains durably identified
for explicit inspection rather than silently adopted or deleted.

`RequestAccountPlaylistDownloads` preserves Read, Preference, RemoveNow and
Prune. A read-only Verify request takes one playlist ID and document ID, hashes
only that recorded file on the worker, and returns the ID in `downloads.verified`
only if its current membership, complete size, identity and digest match.
Read alone does not verify inventory. UI marks are facts checked at that time,
not permanent guarantees against later external deletion.

Progress adds `terminal`, `cancelled` and typed `problem` fields. Setup failure,
missing playlists and external retirement produce terminal notifications;
destruction cancels silently. The observer must keep its existing weak box and
captured account/route/generation guards. Failed and Busy rows require explicit
Retry; definitive unavailable rows do not loop.

This remains an unqualified source foundation. KeepOff/cache mode returns Busy.
The file loader's whole-buffer cache admission is capped at 10 MiB; downloader
traffic through a dedicated reader does not prove a large song is fully cached.
A bounded large-file managed-cache adapter remains required. Windows owned-file
operations, download UI, automatic post-edit prune wiring and native/GUI
qualification also remain open. No C++ compiler or real transfer result is
implied by helper or source checks.


### Download controls

The playlist's Downloads action opens stock GenericBox, SettingsButton and
Radiobutton controls. Keep downloaded offers On, Off and Use default, displaying
the current app-wide default. Changes appear only after the account worker has
confirmed the local-state write. Download all starts a box-owned job; closing the
box or Stop downloading cancels that job while retaining completed files. Retry
is explicit and is available for failed or busy rows after the job ends.

Remove downloads now asks for confirmation and uses the owned-file inventory.
Other playlists, including Recently deleted, retain their references. User
Downloads paths are untouched. Returning to the playlist reloads its view.

Check selected download verifies the selected song through the serialized
account worker. A successful identity, digest, size and membership check adds
the localized Download verified at last check status to the row and its
accessible name. This is a historical observation; removing the file externally
can invalidate it. Refresh or another check clears stale observations. Inventory
presence alone never creates a downloaded mark, and opening the list does not
hash every song.

This private UI slice depends on the owned-transfer adapter's terminal progress
and read-only Verify API. Its compilation, worker integration, actual cloud
transfers and stock light/dark/RTL/accessibility comparisons are not yet proven.
Managed-cache completion, Windows file handling and automatic pruning retain
their separate qualification requirements.

The download composition registers two additional host configurations while
retaining all 63 prior configurations and variants. cases-playlist-download-policy
executes the real codec, reference and ticket-policy fixture; cases-playlist-owned-files
executes owned staging, digest, publication, recovery and removal against a
private QTemporaryDir. Neither is a native cloud, reader-ownership or GUI test.
The resulting 65-gate campaign remains unrun.

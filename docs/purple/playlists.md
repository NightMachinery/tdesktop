# Playlists

The Desktop build includes the shared playlist foundation from purple-core:
strict library and local-state codecs, edits and undo, deterministic merge,
bounded gzip records, record discovery, verified cover checks and complete
sync-step planning. Its contract and focused tests are described in
[the core playlist documentation](../../Telegram/ThirdParty/purple_core/docs/playlists.md).

Desktop includes account-local playlist menus, saved-order playback, global
order and repeat modes, a virtual song list, player-bar navigation, owned
downloads, managed-cache downloads and reference-based retention maintenance.
The integrated source passed production compilation and the complete 66-gate
host campaign. Its fixtures include real temporary-file and encrypted-cache
execution. These results do not establish live Telegram resolution, audible
playback, application lifetimes, download UI or timer behavior in the app.

Application playback and UI qualification, playlist sync transport, Android
playlist parity and the Windows kept-file backend remain unfinished. Sharing
is deferred, and changing the sync chat does not clean records from the old
chat. The source is qualified separately from packaged builds and installation.

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
The library and recently deleted view use pages of at most 25 entries. Song
detail uses the virtual list described below.
Create, rename, copy, delete, restore, remove, reorder and structured Undo submit
typed account-worker edits and display the confirmed reloaded snapshot.
Songs retain saved title, performer and duration without requiring their source
to resolve. Display order comes from the shared core, not the song map's order.
Drag reorder and the virtual list are production-compiled; native interaction
and rendering checks remain pending.

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
along with the UI. Retained downloads have their own controls below. Sync
transport and sharing remain unfinished. The sync line says local-only rather
than claiming a sync succeeded.

The shared settings scope registry makes `menu_item_p`, `keep_downloaded_p` and
private listening app-wide. Automatic playlist sync is per account; its local
sync switch is per account and device. Downloads use per-playlist
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
The separate playlist detail view provides virtual rows and drag reorder;
the player bridge suppresses the unrelated chat hover list for an owned song.
Download controls are described below. Sync and sharing remain follow-ups.

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
syntax checks are registered separately. The fixture passes in the complete host campaign. It does not prove real RPC, stock streaming, actual
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
automatic resume while explicit Play can resume. This composition is production-compiled and host-qualified. Real streaming
traces remain required.

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

## Queue-mode qualification

The integrated queue-mode extension passes its host fixture. This is not
application playback qualification. The fixture covers deterministic
order/permutation checks,
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

The changed player, native hook and stock box are production-compiled and the
fixture passes. Real queue traces across all order/repeat combinations remain
required. Native checks must cover selected-first playback, live mode
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

## Virtual detail list

Song detail replaces pagination with one painted content widget under the stock media-player ScrollArea. It paints the clipped
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
worker, playback facade or timer API changes are needed for this view.

ShowPlaylistBox(controller, playlistId, focusedDocumentId) opens and reads the
bound account playlist, then reveals/focuses a matching saved row. Focus never
starts playback. A row removed by newer edits is not reinserted. Existing
ShowPlaylistsBox remains the library entry.

Source state is limited to the public playback status for its reported row;
other rows say Source not checked. There is no background availability scan or
invented downloaded mark. Current means a delivered playlist row whose document
still matches the native Song player, including a paused item. The list itself
does not scan sources or start downloads; download controls are separate.

Production compilation has passed. A real 2,000-row performance/GUI pass remains
required:
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

KeepOff uses a dedicated native streaming reader and the account's ordinary
big-file cache. It creates no kept or Downloads file, registers no file location,
and writes no playlist library, backup or local inventory. Source and cache
passes use 128 KiB buffers on the main thread. A wake-only relay never touches
readers, devices, sessions or cache; it only schedules the next bounded turn.
The reader receives a detached writable byte span of its Qt buffer.
Local file and memory sources retain main-thread device access. Their loader
bridge safely omits the remote-only downloader attachment optimization, so the
kept-file path also preserves genuine offline/local reuse.

Native slices, headers and serialized cache objects remain bounded. After each
8 MiB interval the pump waits for an asynchronous FIFO cache operation before
continuing, so writes cannot accumulate for the entire song. A cancelled writer
keeps its transfer lease until an asynchronous fence drains its accepted writes,
so repeated cancel/restart also stays bounded. The bridge flushes
remaining slices through the native serializer, drains them the same way, and
lets those dedicated readers skip destructor cache synchronization. Ordinary
readers are unchanged. Cancellation wakes and joins only the relay, then stops
and destroys readers on main before session cache destruction; it does not wait
for cache work or main delivery from a cache worker.

Completion requires a fresh cache-only reader to reread every byte and match the
source pass's SHA256, followed by a fresh native cache-mapping/prefix check.
Cache-only fallback fails instead of reading a local source or the network.
Neither queued writes nor full-in-cache metadata alone establish completion.
Failure, eviction or insufficient cache capacity can prevent that proof and
require explicit Retry. The result records cache contents verified at that
check; ordinary cache eviction may remove them afterward. Row file-verification
marks remain kept-file checks and are not invented for managed-cache rows.

Native Reader admission is positive size at most 510 times 8 MiB (4,278,190,080
bytes), including the native 32-bit addition bound. Larger sizes or a missing
native document cache key return typed Unsupported. The implementation is production-compiled and its real encrypted-cache fixture
passes. Cloud transfer ownership and UI behavior in the application still need
native validation. Windows owned kept-file operations and the documented
recovery limits remain open. Post-edit pruning is integrated as described below;
host cache execution does not establish application download behavior.


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

These controls use the owned-transfer adapter's terminal progress and read-only
Verify API. Production compilation and host worker integration pass; actual cloud
transfers and stock light/dark/RTL/accessibility comparisons remain unverified.
Managed-cache completion is covered by a real encrypted-cache fixture, while
application behavior and Windows file handling need separate qualification.

The complete 66-gate host campaign includes `cases-playlist-download-policy`,
`cases-playlist-owned-files` and `cases-playlist-managed-cache`, preserving the
earlier configurations and variants. They execute the real codec, reference
and ticket policy, owned staging/digest/publication/recovery/removal against a
private QTemporaryDir, and real encrypted-cache Reader/LoaderLocal behavior.
These fixtures do not establish cloud transfer ownership or GUI behavior.

Download status labels use the ordinary `{amount}` placeholder. The language
generator reserves `{count}` for plural keys, so these numeric summaries and
their UI call sites must use the same non-plural tag.

The download-state validators use Qt's const member iterators for byte arrays
and strings, preserving the same identity, digest, token and extension checks
without relying on unqualified iterator lookup.
### Reference-removal retention maintenance

Confirmed local Remove songs, Delete forever and Undo edits request pruning on
the same serialized account worker after the library write succeeds. Every
unpurged playlist, including Recently deleted, protects its referenced owned
files. Pruning preserves unknown future download state and reports a separate
retention status; a cleanup failure cannot turn a confirmed edit into a failed
edit or invite the user to repeat it. The list status explains pending cleanup.
No download metadata is created when the client member was absent.

The existing identity, digest, private-directory and no-foreign-file checks
remain authoritative. Active durable transfer intents may defer cleanup. No
automatic retry loop is added. This hook covers local worker edits; future
account-sync merge delivery must call the same maintenance operation after a
confirmed merge. Native I/O failure and route-retirement checks remain part of
qualification. The service fixture includes actual owned publication, sole
reference removal, another Recently deleted reference and final purge cases.

The passing retention service fixture also covers a changed-digest refusal and
an already-missing-file cleanup. Confirmed edits and Undo stay available when
a mismatched file is retained. Temporary-directory setup failure refuses
all fixture I/O before a fallback path can escape the private test root.

Kept-file verification and removal receive the current account authority, plus
the job authority for registration. The real hash loop checks it between
bounded reads, and removal checks again immediately before its unlink
linearization point. Revocation during a large verification cannot fall
through to an unlink authorized only before hashing. The authority check
does not claim an atomic compare-and-unlink against an unrelated external
filesystem writer. Transfer publication has separate durability and
authority semantics and still needs its own cancellation audit.

The managed-cache fixture refuses invalid temporary-directory setup before any
file path is formed. Asynchronous database completion state is shared with its
callbacks, so a bounded wait failure cannot leave a callback referring to an
unwound stack variable. This fixture has passed actual host execution.
Its native reader links the archive loader's bounded-fill methods and xxHash
alongside the encrypted cache objects.

### Publication authority at the filesystem boundary

Staging verification and publication accept the live transfer authority. Hashing
checks it between bounded reads, including recovery paths. Publication checks
again after the file flush and immediately before creating the no-replace final
link. A cancellation observed before that boundary preserves the stage and
publishes no new final name. Retirement after the link may leave a published
file plus its durable intent for explicit recovery; it cannot claim inventory
commit or successful delivery. This does not provide an atomic compare-and-link
against an unrelated external filesystem writer. The production correction and
real-file fixture pass compilation and host execution. A separate native file
regression revokes authority immediately after a successful fsync and verifies
that no final link is created; removing only the final guard fails the unchanged
assertions. Application cancellation timing remains unverified.

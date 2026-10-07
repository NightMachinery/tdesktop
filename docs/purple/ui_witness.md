# Inert attachment paint witness

The optional witness observes a small part of normal message painting. It uses
an existing `Ui::Text::HighlightInfoRequest`, draws once, and reports a shaped
character cell. It does not change selection, focus, menu decisions or imports.
A cell identifies a character's layout area; it is not an ink-pixel assertion.

## External harness source

Reusable capture, metadata-worker and controller source and synthetic tests are
versioned in the private [Purple test infrastructure repository](https://github.com/NightMachinery/purple-test-infrastructure).
The app-side witness implementation and its product tests remain in this fork.
Read the private repository's component status before use; its source snapshots
and passing inert tests do not imply native qualification.

## Activation and scope

Without both `PURPLE_UI_WITNESS_FD` and `PURPLE_UI_WITNESS_RUN`, startup does
nothing. The first names a canonical inherited descriptor number of at least
three; the second is a public run token of 64 lowercase hexadecimal digits.
The descriptor must be a connected stream AF_UNIX socket owned by the current
user with a same-user peer. The observer duplicates it, makes it nonblocking,
and consumes the valid inherited descriptor. There is no listener, path,
configuration reader, account enumeration or action command.

The first catalog has two whole-text literals, without any entities:

- Slot zero: `PURPLE_WITNESS_BODY`, in the actual Message body String.
- Slot one: `PURPLE_WITNESS_CAPTION`, in the actual Document caption String.

The source must be a canonical, nonsending, nonuploading regular history item
in its asking session's own Saved Messages. Its current attachment must pass
the existing settings-file qualifier: File classification, matching session,
positive bounded size, no upload or active download, and the settings filename.
The item must be the visible container's actual text owner. The current visible
media must own that same document. A marker without the attachment refuses.
Matching text in a reply, another leaf, another item or another receiver does
not establish authority. Neither source text nor document bytes are exported.

Use only an ordinary, ungrouped, inert attachment with exactly the selected
literal as its complete caption/body text, no entities, and no rich page. The
normal ungrouped settings attachment uses the Message body renderer in this
checkout. Document caption construction currently occurs through the grouping
layout path, so a naturally reachable ungrouped slot-one fixture is not
established. The caption hook refuses grouped owners and is not a claim of
caption coverage. Do not construct an album merely to bypass this restriction.
Fixture creation and publication need their own authorization.

The initial contexts are History for the main HistoryInner receiver and Pinned
for the secondary ListWidget receiver. Replies, SavedSublist, AdminLog, scheduled,
preview and all other contexts refuse, even with equal qualifying bytes. This
is a deliberate narrower first catalog, not secondary-context coverage.

At most two ever-registered main or secondary history receivers, two inert
slots, one observation, eight cells, one response and one coalesced dirty
notice exist. IDs are bounded run-scoped equality tokens from a monotonic
counter, independent of private content. Removed targets are retired; receivers
are never recycled. A replacement session or exhausted registry closes the
observer. The application remains usable when the observer closes.

## Paint and lifecycle

The receiver paint scope supplies a weak, canonical item-to-view lookup. The
Message body and Document caption hooks borrow the highlight field only when
its existing value is null. Selection, ripple, reveal, postprocess and competing
highlight paths retain their normal draw behavior and refuse observation.

Admission bounds the whole inert leaf before requesting its first nonspace
character, range `[0, 1)`. Only a matching QWidget paint device with an integer
axis translation and no explicit painter clip is admitted. A single paint
region must cover the entire settled viewport and all cells. No partial-paint
union, cached image, forced repaint or diagnostic hit test becomes a witness.
The record becomes observable only after the enclosing paint has finished.

Each Observe and Validate re-resolves the weak controller and session, canonical
item, actual visible media and String. It compares document identity, filename
and size, item date internally, font, interface scale, direction, geometry,
selection/readiness and focused item. Layout, repaint, resize, refresh, removal,
id changes, content changes, clear/unload, language and palette changes dirty
the observation. Existing focus mutations have a small dirty hook. Qt focus,
input, scroll, window movement, native-parent and visibility events also dirty
it. Notifications supplement the final current-state comparison.

Menu provenance comes only from the actual qualified Import action and the
final asking receiver's menu construction. Reactions and specialized attached
menus refuse. The ordinary popup must finish its show animation and retain its
actual action widget and receiver parent. Its NSView handle resolves to its own
NSWindow number, distinct from the main window. Keyboard origin must have had
actual asking-widget focus on the target at construction. No observer calls an
importer, hit producer, focus setter or action callback.

## Wire version one

Frames use a four-byte big-endian body length followed by one ASCII JSON object.
Requests have a maximum of 1024 bytes and exactly these keys: `schema`, `op`,
`facet`, `kind`, `slot`, `duration`, `run`, `acquisition`, `burst`, `nonce`,
`receiver`, `receipt`. Schema is one. Operations are Observe=1, Validate=2,
Close=3. Facets are Text=1 and Menu=2. Kinds are Main=1 and Secondary=2.
Slots are zero or one; duration is one to 8000 milliseconds. Token fields are
64 lowercase hexadecimal digits. The zero receiver token asks for the unique
registered receiver of that kind. Duplicate keys, unknown fields, trailers,
wrong types and parser limits refuse before successful request conversion.
Depth is at most six, arrays eight, object keys sixteen, nodes 128 and scalars
128 bytes. Escaped strings and noncanonical numbers are outside this grammar.

Replies are at most 16 KiB. Every irrevocably queued frame has an ascending
hexadecimal sequence string, starting at one. Hello has schema/type/sequence/
run/catalog, with type one and catalog one. Observation replies have schema,
type two, sequence, header, status, code, remaining, authority and receipt.
The header carries run/acquisition/burst/nonce. Success is status one and code
zero. Static failure codes are receiver ambiguity=2, pending=3 and stale=4.
A refusal has empty authority and a zero receipt. No private error text leaves.

Authority contains catalog, receiver/controller/item/document equality tokens,
slot/facet/kind, epoch, focus, selected, keyboard, window/frame, viewport,
geometry, scale, cells, popup, input and enabled. Rectangles are four integer
fixed-point values at 256 units per logical point. Scale 2048 means two device
pixels per logical point. Focus is the history's current focused item:
none=0, target=1, other=2. Keyboard separately records actual receiver focus.
Selected is zero in this whitelist. Text has an empty popup, input minus one
and enabled zero. A menu has its actual window number, frame and root number;
input is Mouse=0 or Keyboard=1 and enabled is one. This is a bounded application
scene subset; the native supervisor must independently bind its complete scene.

The receipt is SHA-256 of the public domain `PURPLE_UI_WITNESS_1\n` and Qt's
compact authority JSON. It is an equality check, not a permission token. A
Validate request uses a fresh nonce while retaining the original run,
acquisition, burst, receiver, facet, kind, slot, duration and receipt. Complete
stable authority must match. Validation never renews the original deadline.
The final response lasts at most 150 milliseconds within that deadline; this
is not a promise that native work completes in that time. The consumer measures
the original deadline from its own request start and refuses ended bursts.

Dirty frames have type three, schema/sequence, a union receiver mask and epoch.
Mask three is All. Replaced pending notices do not consume a sequence. Dirty
state irreversibly blocks validation, including after a partial stale response.
Every partial write checks expiry. Ordering ambiguity, overflow, peer EOF,
partial-read timeout or session lifetime of 45 minutes closes the observer.
A continuously draining supervisor must observe notices and terminal state;
a disposable metadata worker cannot own or conceal this channel.

A separate read-only LoadedProxyMode request uses operation four and exactly
`schema`, `op`, `run`, `nonce`, `peer`, `requestSequence` and `requestedAt`.
The request sequence begins at one and increases by one for the same peer token;
no more than 32 requests are accepted in an observer run. `requestedAt` is a
canonical 13-digit Unix millisecond string. The observer checks the run and
fresh nonce against the active session, checks the echoed peer and ordered
sequence, and limits the request and queued reply to one second from that
original time. At admission it fixes a monotonic deadline to the earlier of the
remaining one-second window and the 45-minute session end. The synchronous
getter must finish before that deadline. A later wall-time check can refuse the
reply or shorten its queued deadline, never extend the admission deadline. It
reads `Core::App().settings().proxy().settings()` on the
normal UI thread when handling the request. Application startup calls
`StartUiWitness()` after `Local::start()` has loaded app settings, and each
query reads the current loaded getter rather than a startup cache.

The reply is type four and contains only schema/type/frame sequence, run,
nonce, peer, request sequence, the requested and observed times, and a mode
integer: System=0, Disabled=1, Enabled=2. It has no authority, receipt, lease,
receiver, geometry, host, port, proxy-list or credential fields. The peer token
is correlation only. A consumer must bind the event to the actual observed
process birth and source, then enforce the original request-time and observer
lifetime from its own monotonic clock. Type four cannot renew a stopped source
or establish input or geometry authority. Its private scalar decoder accepts a
bounded flat object and refuses nested values before general JSON decoding. The
channel owner must still revoke and close on terminal protocol or source failure.
Hello and the type-two and type-three wire objects remain unchanged.

## Qualification limits

The mac adapter is limited to one screen, exact two-times device scale and
integer public Qt/AppKit frame agreement. Native association, physical input,
full scene containment and application receiver/menu lifetimes need independent
qualification. Offscreen String tests establish renderer and codec behavior,
not an actual window witness. Current source fixtures connect the real framing
and equality code to an inert decoder, the existing RAM mask, and the full
emitted typed Lua with fake HS. They do not enable a native consumer.

The existing helper's static observation whitelist and broad L7 schema do not
admit this reduced producer. A separately reviewed reduced adapter must keep
original expiry, complete source equality, physical origin, supervised notices
and actual scene binding. It must not fabricate group/day/post/fixture receipts.
Its keyboard packet currently requires selection, which this producer refuses.
The adapter remains unqualified and disabled until combined source review.

Grouped/day/rich/editor/filename/card/corner/cache/save/status/timer/async facets,
negative menu omissions, Files/Downloads/Stories/global/other receivers and
keyboard focus acquisition remain outside the first catalog. Public controls
and root-targeted store observations use their existing interfaces. An ordinary
autosend document is not a proper sync record. Later facets need explicit scope
and review; a conservative refusal does not complete required coverage.

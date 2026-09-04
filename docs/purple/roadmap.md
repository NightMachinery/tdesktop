# Roadmap

Where the Android port stands, and what is left. The desktop fork is the
reference implementation throughout: when a behaviour is described here in one
line, `work_mode.md` has the long version and the reasoning.

Anything written but not yet driven on a device is in [todo.md](todo.md)
rather than described as done here.

Milestones are numbered A1 upwards because they are the *Android* ones. Each
lands as its own commits in both repositories, with the documentation changed
in the same commit as the behaviour.

## Done

**A1 - the seam.** The Work Mode core compiled through the NDK into
`libpurplecore.so`, so both clients run the same C++ and a `settings.toml`
written on the desktop means the same thing on the phone. `PurpleGate` is the
one file that knows both what a `TLRPC.Dialog` is and what the core wants.
Settings arrive by import from Saved Messages.

**A2 - the view.** The chat list shows what the active preset lets through, and
a picker chooses the preset. Hiding is a view and never an edit: a hidden chat
is still pinned, still in search, still in the forward picker. Pin dragging is
refused while a preset runs, because a drop is uploaded as the whole order and
would drop the hidden chats' pins from the account.

**A3 - silencing, the badge and the folder strip.** A preset can silence what it
lets through; a hidden chat neither notifies nor lights the launcher icon; and
the strip shows the folders the preset names, in the order it names them, with
reordering refused while it is restricted.

**A4 - the folder mechanism.** A `folders` entry's `notify_p`, `badge_p` and
`include_in_main_view` all work, including pulling a folder's chats into the
view when they are archived. They were one milestone rather than three because
all three need the same question answered off the UI thread - whether a given
chat is inside a given folder, right now.

## Next

**A5 - the rest of Work Mode.** Everything is done except the launch-time offer
of a settings import, and that one is **held back on purpose rather than left
over**. [sync.md](sync.md) owns the feature and says why: an offer that appears
on every launch of a machine you never sync is worse than no offer, and the rule
for suppressing it is easier to write once there is real use to look at. The
manual path it is meant to improve has not been exercised end to end even once
yet. Building it now would be guessing at the suppression rule, on Android
first, where the desktop does not have it either.

The criterion for starting it is therefore not effort: it is the desktop
export/import round trip having been used for a while.

Landed so far: hot reload of `settings.toml`, filing a chat into a list from the
chat list, the two things that move on a clock rather than on an edit - peek and
the schedule - the "until" decisions with the line that explains what is
deciding a chat, the `[recent]` close buffer, and extra views. The middle three
are one family - three ways a chat is decided without the preset having a say -
and they meet in one helper, which is why each cost less than the one before.

Extra views were the last structural piece and the cheapest surprise in the
port: an invented tab is a synthetic `DialogFilter` that only the display
accessor can see, so A3's two-accessor rule turned out to be what made them
nearly free. Membership rides one bit per view on the per-chat answer the gate
already caches, which is the only shape a predicate called once per chat per
sort could afford. Peek was almost free, and that is worth recording as evidence
the seam is in the right place: the engine already reveals everything while its
`peeking` flag is set, and the same function the chat list has always called
answered differently the moment the bridge set it. What the port had to add was
the folder strip, the reorder guard, a timer and a checkbox.

The current gap is listed precisely under "Not ported yet" in `work_mode.md`,
which shrinks as each lands.

**A6 - the fork's own defaults.** Everything above makes the phone behave like
the desktop. This one is about the things the fork should decide differently
from upstream regardless of Work Mode.

Landed so far: the two defaults a fresh install needs, both in
[defaults.md](defaults.md) - the Archive row off the top of the chat list, and
the background connection on. They went together because they are one question,
"what should this do before anybody configures anything". The second was the
one with a surprise in it: it is three places that have to agree on the same
answer, not the single line this file used to claim, and changing only the real
one leaves the switch rendering off while the connection runs.

Left:

- **Local Premium, ported.** The desktop fork already has this, and
  [premium.md](premium.md) is the design rather than a starting point: a small
  `Purple::` helper consulted at a handful of named call sites, governed by
  `[premium] enabled` in `settings.toml` and on by default. What it explicitly
  does **not** do is make the session claim to be Premium - that would light up
  a couple of hundred call sites offering things the server refuses, and put a
  badge on your own profile that nobody else can see. Android should follow the
  same shape, and the same settings key, so one file still means one thing in
  both clients.

  The per-feature split has to be redone rather than copied, because which gates
  are client-only differs by client: sponsored messages, the translate-chats
  gates, and the account limit each live somewhere else here. Sponsored messages
  look portable - `getSponsoredMessages()` carries no Premium check at all, so
  the client is the only thing that would stop asking, which is exactly the
  desktop's reasoning.

  One candidate the desktop does not have: `lockFiltersInternal()` locks every
  folder past the non-premium limit, and locked folders are exactly what a
  `folders` preset needs. It fits the doc's own criterion - the client declining
  to use something it already holds - but the folder *count* limit is enforced
  by the server too, so this may unlock folders that already exist without
  allowing new ones. Establish that rather than assume it, and re-check it
  against the A3 and A4 folder behaviour, since the folder code reads the
  Premium state in several places.

- **A Purple identity, off by default.** The app installs alongside official
  Telegram under its own application id, but on screen it is indistinguishable
  from it - the chat list header and the notifications both say Telegram. A
  setting, unchecked by default, to use the fork's own name and launcher icon
  instead. Off by default because looking like stock is sometimes the point.

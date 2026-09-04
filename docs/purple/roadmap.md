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

- **Local Premium, ported.** Landed, apart from what turned out not to exist -
  [premium.md](premium.md) has the Android section and the reasoning. `[premium]
  enabled_p` reaches the phone through the bridge, so one settings.toml still
  means one thing in both clients, and sponsored messages and whole-chat
  translation are unlocked here as they are on the desktop.

  The per-feature split did have to be redone rather than copied, and two of the
  desktop's four did not survive the crossing. The account limit has **no gate
  to remove**: every site that offers "Add Account" tests a constant of 4 with
  no Premium check, so everyone already has four, and going further means
  resizing every per-account array - adding capacity, which is not what this
  feature is. Exact last seen is skipped on the strength of the measurement
  already in that document: one contact in 1913 held a recoverable value, and
  the *off* path of such a loop destroys real ones.

  Going the other way, Android has a candidate the desktop does not:
  `lockFiltersInternal()` locks every folder past the non-Premium limit, and
  those folders are already in hand. That is unlocked, and it matters more here
  than upstream - a `folders` preset naming a locked folder is asking for a tab
  the app will not draw. It does not touch the folder *count* limit, which the
  server enforces on creation.

- **A Purple identity, off by default.** The app installs alongside official
  Telegram under its own application id, but on screen it is indistinguishable
  from it - the chat list header and the notifications both say Telegram. A
  setting, unchecked by default, to use the fork's own name and launcher icon
  instead. Off by default because looking like stock is sometimes the point.

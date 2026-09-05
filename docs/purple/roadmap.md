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

**A6 - the fork's own defaults.** What this fork decides differently from
upstream regardless of Work Mode, all of it in [defaults.md](defaults.md): the
Archive row off the top of the chat list, the background connection on because
without push nothing else here can notify, Local Premium ported, and the app
calling itself by its own name with its own launcher icon on offer.

Two of the four were not what they looked like. The connection default is three
sites that must agree, not the one line this file used to name - change only the
one that decides the behaviour and the switch renders off while the connection
runs. And the identity was half a bug rather than a missing feature: the fork
had renamed itself in `strings.xml` long ago and the launcher had always shown
it; only the running app disagreed, because `LocaleController` reads the
downloaded language pack ahead of its own resources and Telegram's pack carries
`AppName`.

Local Premium lost two of the desktop's four and gained one the desktop has no
equivalent of. The account limit has no gate to remove - every site that offers
"Add Account" tests a constant of 4 with no Premium check - and exact last seen
was measured to be worth a single contact in 1913. Locked folders are the new
one, and this fork needs them more than upstream does, since a `folders` preset
naming a locked folder asks for a tab the app will not draw.

## Nothing after A6

The port is done. What is left is not a milestone:

- A5's **launch-time import offer**, held back on purpose until the manual
  export/import path has been used for a while. The criterion is above, and
  [sync.md](sync.md) owns the decision.
- Two small pieces waiting to land with something else rather than alone: the
  folder-tab half of `hide_scope`'s default, and the chat-list mark for a row
  that is only present on a clock, which `[recent] style` and a "show until"
  both want.
- The verification debt in [todo.md](todo.md) - behaviour that is written and
  reasoned about but has not been driven on a screen. It was waiting on a
  renderer, and is not any more: the laptop runs the APK natively on its own
  GPU, so the popups that killed the build box's software rasteriser are
  reachable.

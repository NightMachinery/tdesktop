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

**A5 - the rest of Work Mode.** Done, including the launch-time offer of a
settings import, whose Android half landed on 2026-09-06. The thing that had
been held back was never the code but the suppression rule, and the rule is now
decided: **one offer per message, ever**. Each account remembers the id of the
newest `settings.toml` message it has already had an opinion about, and a
message earns a line on the screen only if its id is higher than that and its
date is later than the local file's - so the machine you never sync sees each
file you post exactly once and then nothing. [sync.md](sync.md) owns the feature
and carries the reasoning.

It went to Android first on purpose. The phone is the machine you pick up after
editing settings somewhere else, so it is where the offer is worth having and
where the rule gets tested against real use. The desktop half follows, with the
same rule.

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

- A5's **launch-time import offer**, whose Android half landed on 2026-09-06
  under the "one offer per message, ever" rule described above. What is left is
  the desktop half, which gets the same rule and the same wording;
  [sync.md](sync.md) owns both.
- The two small pieces that waited here - the folder-tab half of
  `hide_scope`'s default, and the chat-list mark for a row that is only
  present on a clock - landed on 2026-09-06, and both have since been seen on
  a screen in both directions.
- The verification debt in [todo.md](todo.md) is paid, apart from the two
  surfaces this emulator cannot reach. The laptop runs the APK natively on its
  own GPU, and two runs - 2026-09-07 and 2026-09-08 - cleared the backlog: the
  list box and its verdict line in every reading, the three "until" spans, the
  preview menu, the App Icon picker, the folder unlock, the peek reorder guard
  in all three of its cases, sponsored on both surfaces, and the first
  empirical proof that a preset suppresses a notification rather than merely
  looking as though it would. No ANR, crash or segfault anywhere. Each run
  found one real bug and fixed it the same night: the preview menu's entry
  opened nothing, because it posted the dialog into a fragment still being
  dismissed; and an extra view lost its pinned order on every cold start,
  because the pins were resolved against a dialog list that had not loaded yet
  and nothing retried them.

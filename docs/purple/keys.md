# Modal keyboard shortcuts

Enable **Modal keyboard shortcuts** under **Settings -> Purple**. When enabled,
each main window opens in **Normal** mode with a persistent **modal status line**
docked along the bottom edge (28 px, reserved beneath the main layout so it
never covers messages or the composer).

## Modes and status line

- **Normal** (green badge, default when enabled): keys dispatch commands.
  Switching chats, folders, or panes stays in Normal mode while clearing any
  in-flight prefix or count.
- **Insert** (blue badge): press `i` or `a`, start a reply or edit hint, open
  search, or click directly inside the message composer to type. Ordinary
  typing, IME composition, and Telegram shortcuts work normally. Press `Esc`,
  `C-[`, or `C-g` to return to Normal mode.
- **Transient capture and layer badges:** the status line updates at 0 ms to
  `LEADER` (indigo) after `SPC`, `LOCAL` with the active context name (teal,
  for example `LOCAL · CHAT`) after `,` or `SPC m`, `HINTS` (purple) during
  message/URL hints, `M-x` (violet) in the command palette, `KEY?` (amber)
  during `SPC h k`, and `HELP` / `FILTER` while the cheatsheet, bindings list,
  describe card, or stock shortcuts layer is open. While a prefix, which-key
  drawer, hint capture, or layer is open, the status bar replaces the
  Normal-mode hint on its right side with that surface's active keys.
- **Pending counts and prefixes:** numeric counts (`5j`, `2C-d`) and pending
  chords (`SPC b -`, `, -`, `y -`) echo immediately on the status line beside
  their prefix group label (`+buffer / chat`). Pressing `DEL` inside a prefix
  backs up one stroke; `Esc` or `C-g` cancels. Unknown endings report
  `<chord> is undefined` on the status line and never fall through to Normal
  mode. Unavailable commands report what they need.
- **Unread unmuted chat counter:** the status line shows a live `DM N · GP M · CH K`
  pill counting unread unmuted chats (not messages), broken down into direct
  messages and bots (`DM`), groups and forum groups (`GP`), and channels (`CH`).
  Muted chats without mentions, archived chats, Saved Messages, and chats hidden
  by the active Work Mode preset are excluded.

With a non-ASCII keyboard layout such as Persian, Normal mode chords and
`C-[` follow physical U.S. key positions while Insert mode continues typing in
the active layout. On macOS, `M-` (Option) chords such as `M-x`, `M-p`, and
`M-n` also resolve by physical key so Option symbols do not leak into inputs.

## Docked which-key drawer

Pausing ~280 ms inside a prefix opens a docked which-key drawer flush above the
modal status line (reusing stock `Ui::KeysLegend` keycaps and theme colors).
The header displays the pending chord, group or localleader title, and
`DEL back · ESC cancel`. The responsive multi-column grid lists only applicable continuations, placing
`+group` prefixes in accent color above a hairline divider and merging duplicate
alias keys (such as `SPC / :` or `, / RET`) into one command row. Pressing `,`
or `SPC m` where no context bindings apply reports `No local keys here` on the
status line and swallows the next keystroke.

## Keymap overview

Every command has a canonical home in a Doom-aligned `SPC` leader group, a Vim
prefix (`g`, `y`, `[`, `]`, `C-w`), or a localleader context map (`,` / `SPC m`),
with at most three strokes per chord:

- **Navigate:** `j` / `k` move the chat cursor (in the chat list) or scroll the
  conversation (in a chat), repeatable with counts (`5j`). `J` / `K` (`C-j` /
  `C-k`) step through chats from any pane. `RET` / `o` open the highlighted
  chat. `h` / `l` (`SPC w h` / `SPC w l` / `C-w h` / `C-w l`) switch between
  the chat list and conversation; `SPC w w` (`C-w w`) toggles panes. `C-d` /
  `C-u` scroll half a page, `C-f` / `C-b` (`]` / `[` `m`) scroll a full page,
  `G` / `] ]` jump to latest, `] u` / `g u` jump to unread, `] f` / `[ f` step
  folders.
- **Leader (`SPC`):**
  - `SPC SPC`, `M-x`, `:`, `SPC :`: command palette (`M-x`).
  - `SPC ,`, `SPC RET`, `SPC b b`: chat switcher palette.
  - `SPC u`, `SPC b I`: unread unmuted chats buffer (iBuffer).
  - `SPC b`: active chat (`b i` info, `b m` chat menu, `b p` pinned, `b v`
    preview, `b s` scheduled, `b d` calendar, `b u` attach files, `b w`
    wallpaper toggle).
  - `SPC o`: open views (`o s` Settings, `o S` Saved Messages, `o a` Archive,
    `o c` Contacts, `o p` Calls, `o d` Downloads, `o m` Main Menu, `o u` Music,
    `o l` Playlists).
  - `SPC n`: new (`n g` New Group, `n c` New Channel, `n p` Add Contact).
  - `SPC p`: playback (`p p` play/pause, `p s` stop, `p n` / `p N` next/prev
    track, `p f` / `p b` seek forward/back, `p m` Music, `p l` Playlists;
    also `] t` / `[ t`).
  - `SPC s`: search (`s s` or `/` search messages, `s c` chat palette).
  - `SPC t`: toggles (`t b` chat background artwork visibility, `t n` Night Mode).
  - `SPC w`: panes (`w h` chats, `w l` conversation, `w w` toggle).
  - `SPC a 1` through `SPC a 6`: switch signed-in accounts.
  - `SPC h`: help (`h c` or `?` cheatsheet, `h b` or `SPC ?` all active
    bindings, `h k` describe key, `h f` describe command, `h s` Telegram stock
    shortcuts, `h r` reload `keys.edn`).
- **Localleader (`,` and `SPC m`):**
  - In a chat or topic (`:chat`): `, i` info, `, m` menu, `, p` pinned, `, s`
    scheduled, `, d` calendar, `, a` attach, `, r` reply hint, `, e` edit hint,
    and 2-stroke shared media (`, v` media, `, u` music, `, f` files, `, l`
    links, `, o` voice/round, `, g` GIFs, `, q` polls).
  - In the chat list (`:chats`): `, p` preview, `, m` chat menu, `, a` archive,
    `, f` / `, F` next/previous folder.
  - In Music (`:music`): `, p` play/pause, `, s` stop, `, n` / `, N` next/prev
    track, `, f` / `, b` seek forward/back, `, l` playlists.
- **Yank (`y`) and message hints (`g`, `f`):**
  - `y y` / `y m` copy visible message text, `y l` copy message link, `y u`
    copy URL in message text.
  - `f` / `g o` open visible URL in message text.
  - `g s` select message, `g m` open message context menu, `g r` reply to
    message, `g e` edit message, `g u` jump to unread.
  - Hint tags render in high-contrast amber-gold with crimson typed-prefix
    highlighting and follow Telegram's copy and selection restrictions.
- **Insert mode (`:insert`):** `C-[` enters Normal mode, `M-x` opens the
  command palette and restores focus to the composer on cancel.

## Help views and `M-x` command palette

- **`?` (Cheatsheet) and `SPC h b` / `SPC ?` (All Active Bindings):** built on
  stock `Ui::LayerWidget` and `Ui::KeysLegend` keycaps. `?` presents six
  curated task cards with live-resolved bindings and a footer showing the
  `keys.edn` path and status. Press `TAB` (or click the header tabs) to switch
  to the exhaustive active binding list grouped by scope, with orderless
  filtering (`/`), `j`/`k` scrolling, and `q` / `Esc` to close.
- **`SPC h k` (Describe Key) and `SPC h f` (Describe Command):** inspect any
  chord without executing it, or pick any command to see its availability, why
  it is unavailable if so, and every map where it is bound.
- **`M-x` / `:` / `SPC :` / `SPC SPC` (Command Palette):** upper-third anchored
  layer (never jumps vertically as results narrow) with an `amx` + `orderless`
  ranking engine: available commands first, then title matches ahead of
  group/id-only matches, with `amx` top-7 recency, invocation frequency,
  shorter labels, and A-Z breaking ties inside each tier (persisted in
  `mx-history.json`). Space-separated query terms match in any order as
  substrings, safe regexes, or word initialisms (`opm` for Open pinned
  messages), highlighting matches in both the title and the category/command-id
  subtitle alongside right-aligned live keycaps. Navigate with `C-n`/`C-p`,
  `C-j`/`C-k`, arrows, or `Tab`/`S-Tab`; step input history with `M-p`/`M-n`.

## Custom bindings (`keys.edn`)

Purple bundles [default-keys.edn](../../Telegram/Resources/purple/default-keys.edn)
in the shared `evilify` EDN format. When modal keys are enabled and no user
keymap exists yet, Purple creates a commented starter `keys.edn` beside
`settings.toml` and watches it for live reload on save (or reload manually with
`SPC h r`).

```clojure
{:localleader ["," "SPC m"]
 :normal {"SPC o a" nil
          "C-c m"   :open-music
          "SPC b"   "chat"}
 :chat   {"k"       :open-pinned-messages}
 :insert {"C-["     :enter-normal-mode
          "M-x"     :command-search}}
```

Maps are `:normal`, `:insert`, `:chat`, `:chats`, and `:music`. Optional
top-level keys are `:localleader` (vector of chord strings) and `:keyless`
(vector of deliberately unbound command keywords). String values inside a map
label a prefix in which-key and the status line. Uppercase letters (`"G"`)
normalize to `S-g`. Files up to 32 KiB and 256 entries are validated atomically:
any invalid entry keeps built-in defaults active and displays the exact reason
on the modal status line and cheatsheet footer.

## Unread buffer, avatars, Zen fullscreen, privacy screen, and topics

- **Unread unmuted chats buffer (`SPC u`, `SPC b I`):** an iBuffer-style list of
  chats that currently have unread unmuted notifications, showing profile
  pictures, `DM` / `GP` / `CH` tags, unread counts, and message previews.
  Navigate with `j`/`k`, `C-n`/`C-p`, `g g`, `G`, filter orderless with `/`,
  refresh with `g r`, mark with `m` / `u` / `U` / `t` / `* d` / `* g` / `* c`,
  open with `RET` or `o`, and mark read with `r` or `x`. Marking more than one
  chat as read asks for an explicit `y` / `RET` confirmation first because
  marking read sends read receipts.
- **Profile pictures in modal menus:** circular peer avatars appear beside
  people, groups, channels, and accounts in the unread buffer (`SPC u`), chat
  palette (`SPC ,`), and `M-x` peer-scoped commands.

## Upstream hooks

- `core/application.cpp`: delegates key press, shortcut override, paired key
  release, window deactivation, and IME events to `Purple::HandleModalKeys`.
- `mainwindow.cpp`: attaches the modal status line and docked which-key drawer
  in `finishFirstShow()`, reserves status-bar height beneath `_main` in
  `updateControlsGeometry()`, and raises the modal chrome beneath lock and
  layer overlays in `fixOrder()`.
- `core/shortcuts.h`, `dialogs/dialogs_widget.*`, `history/history_widget.*`,
  `history/history_inner_widget.*`, `history/view/history_view_*`,
  `ui/boxes/calendar_box.cpp`, `window/window_main_menu.cpp`, and
  `window/section_widget.cpp`: existing focus, visible message hint, calendar,
  main menu, and chat background hooks.

## Open chats with the keyboard

`SPC ,`, `SPC RET`, or `SPC b b` opens the chat palette. Search chats, archived
chats, channels, and contacts by name, username, phone number, or contact note.
Use Up or Down to move and Enter to open the highlighted chat.

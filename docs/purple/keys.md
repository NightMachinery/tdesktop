# Modal keyboard shortcuts

Enable **Modal keyboard shortcuts** under **Settings -> Purple**. When enabled,
each main window opens in **Normal** mode with a persistent **modal status line**
docked along the bottom edge (28 px, reserved beneath the main layout so it
never covers messages or the composer).

## Modes and status line

- **Normal** (green badge, default when enabled): keys dispatch commands.
  Switching chats, folders, or panes stays in Normal mode while clearing any
  in-flight prefix or count.
- Hold a single-key motion such as `j`, `k`, `J`, `K`, `C-d`, or `C-u`
  to repeat it. A numeric count applies to the first press. Changing focus
  or leaving Normal mode ends the motion.
- In history and topics, a tap of `j` or `k` eases one step. The first repeat
  starts continuous pixel scrolling, and release stops it with a short ease-out.
  The cheatsheet, notice bodies, and Customize review scroll the same way.
  Repeated half-page and page motions retarget the current scroll animation.
  Lists and pickers still move one row at a time. Turning off Animations
  restores discrete scroll steps.
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
modal status line. Its rounded top edge uses the window theme and spans the
window width. Keycaps reuse the stock `Ui::KeysLegend` style.
The header displays the pending chord, group or localleader title, and
`DEL back · ESC cancel`. The responsive multi-column grid lists only applicable continuations, placing
`+group` prefixes in accent color above a hairline divider and merging duplicate
alias keys (such as `SPC / :` or `, / RET`) into one command row. In the chat
list, the folder rows also show the bare folder shortcuts when bound. Pressing `,`
or `SPC m` where no context bindings apply reports `No local keys here` on the
status line and swallows the next keystroke.

## Keymap overview

Every command has a canonical home in a Doom-aligned `SPC` leader group, a Vim
prefix (`g`, `y`, `[`, `]`, `C-w`), or a localleader context map (`,` / `SPC m`),
with at most four strokes per chord:

- **Navigate:** `j` / `k` move the chat cursor (in the chat list) or scroll the
  conversation (in a chat), repeatable with counts (`5j`). `C-j` / `C-k` and
  `] c` / `[ c` step through the visible chat list from either pane.
  `J` / `K` step the chat cursor from a conversation. In the main chat list,
  they move to the next or previous folder, keep list focus and wrap past the
  ends. Use a count (`3J`) or hold the key to advance through visible folders.
  Work Mode-hidden folders are skipped. Archive and forum topic lists show a
  status message when you try to switch folders with `J` / `K`.
  `RET` / `o` open the highlighted row from the list or after one of those
  steps. The status line shows `RET open` while the same row stays highlighted.
  Moving the pointer over the list, clicking, or switching folders clears the
  cursor picked from another pane. `h` / `l` (`SPC w h` / `SPC w l` / `C-w h` / `C-w l`)
  switch between the chat list and conversation, and `SPC w w` (`C-w w`)
  toggles panes. `C-d` /
  `C-u` scroll half a page, `C-f` / `C-b` (`]` / `[` `m`) scroll a full page,
  `G` / `] ]` jump to latest, `] u` / `g u` jump to unread, `] f` / `[ f` step
  through folders with wrapping when the main chat list is focused.
- **Leader (`SPC`):**
  - `SPC SPC`, `M-x`, `:`, `SPC :`: command palette (`M-x`).
  - `SPC ,`, `SPC RET`: chat switcher palette. `SPC b b`: recently viewed chats.
  - `SPC u`, `SPC b I`: unread unmuted chats buffer (iBuffer).
  - `SPC z`, `SPC t p`: privacy screen across all Purple windows.
  - `SPC b`: active chat (`b i` info, `b m` chat menu, `b p` pinned, `b v`
    preview, `b s` scheduled, `b d` calendar, `b u` attach files, `b w`
    wallpaper toggle, `b t` switch topic, `b T` unread topics, `b n`/`b N`
    next/previous topic, `b l` last topic, `b c` new topic or bot message,
    `b h` topic list).
  - `SPC o`: open views (`o s` Settings, `o S` Saved Messages, `o a` Archive,
    `o c` Contacts, `o p` Calls, `o d` Downloads, `o m` Main Menu, `o u` Music,
    `o l` Playlists).
  - `SPC f e d`: Customize Purple settings. `:customize-settings` opens the
    same view from the command palette.
  - `SPC n`: new (`n g` New Group, `n c` New Channel, `n p` Add Contact).
  - `SPC p`: playback (`p p` play/pause, `p s` stop, `p n` / `p N` next/prev
    track, `p f` / `p b` seek forward/back, `p m` Music, `p l` Playlists;
    also `] t` / `[ t`).
  - `SPC s`: search (`s s` or `/` search messages, `s c` chat palette).
  - `SPC t`: toggles (`t b` chat background artwork visibility, `t n` Night Mode,
    `t z` Zen fullscreen chat, `t p` privacy screen).
  - `SPC w`: panes (`w h` chats, `w l` conversation, `w w` toggle, `w m` Zen
    fullscreen chat, `w z` toggle the chat list in Zen).
  - `SPC a 1` through `SPC a 6`: switch signed-in accounts.
  - `SPC h`: help (`h c` or `?` cheatsheet, `h b` or `SPC ?` all active
    bindings, `h k` describe key, `h f` describe command, `h s` Telegram stock
    shortcuts, `h r` reload `keys.edn`).
- **Localleader (`,` and `SPC m`):**
  - In a chat or topic (`:chat`): `, i` info, `, m` menu, `, p` pinned, `, s`
    scheduled, `, d` calendar, `, a` attach, `, r` reply hint, `, e` edit hint,
    2-stroke shared media (`, v` media, `, u` music, `, f` files, `, l` links,
    `, o` voice/round, `, g` GIFs, `, q` polls), and topic commands (`, t`
    switch topic, `, T` unread topics, `, n`/`N` next/prev topic, `, b` last
    topic, `, c` new topic or bot message, `, h` parent topic list). Also `] T`, `[ T`, `] U`
    (next unread topic), and `g ^` (parent topic list).
  - In the chat list (`:chats`): `, p` preview, `, m` chat menu, `, a` archive,
    `, f` / `, F` next/previous folder.
  - In Music (`:music`): `, p` play/pause, `, s` stop, `, n` / `, N` next/prev
    track, `, f` / `, b` seek forward/back, `, l` playlists.
- **Yank (`y`) and message hints (`g`, `f`):**
  - `y y` / `y m` copy visible message text, `y l` copy message link, `y u`
    copy URL in message text.
  - `f` / `g o` open a visible URL in plain or rich message text. In a rich
    message, expand a Details block before hinting links inside it.
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
  group/id and alias matches, with `amx` top-7 recency, invocation frequency,
  shorter labels, and A-Z breaking ties inside each tier (persisted in
  `mx-history.json`). Space-separated query terms match in any order as
  substrings, safe regexes, or word initialisms (`opm` for Open pinned
  messages), highlighting matches in both the title and the category/command-id
  subtitle alongside right-aligned live keycaps. Matching aliases appear in
  the subtitle when they help find a command. Try `music play` for playback
  toggle. Navigate with `C-n`/`C-p`,
  `C-j`/`C-k`, arrows, or `Tab`/`S-Tab`; step input history with `M-p`/`M-n`.
  Hold the arrow, Control, or Alt motions to repeat them. `F6` switches between
  the search field and results. With modal input enabled, hold `j` or `k` to
  move through the focused results.

With modal keyboard shortcuts off, **Option+X** on macOS (**Alt+X** elsewhere)
still opens M-x in an active, unlocked account window. The palette includes
`:keys-help`, `:cheatsheet`, `:command-describe`, and
`:toggle-privacy-screen`. Commands that rely on Normal mode show their
requirement in the results. The shortcut yields to IME composition, popups,
and dialogs, and is unavailable under the passcode or privacy screen.

Run `:modal-input-toggle` to change **Modal keyboard shortcuts** for all windows.
It has no default binding. Turning it off closes modal shortcut layers and
clears pending keys. Drafts are preserved. Turning it on starts Normal mode.
To type the usual Option+X character while modal input is off, disable
**Option+X opens commands with modal keys off** under **Settings > Purple >
Privacy & controls** or in **Customize settings**.

## Customize settings

Press `SPC f e d` in Normal mode or run `:customize-settings` in the command
palette. Which-key shows **files** after `SPC f`, **edit settings**
after `SPC f e`, and **Customize Purple settings** on `d`. The cheatsheet
and binding reference use your active keymap. **Settings > Purple >
Customize settings…** also opens the editor with modal keys off.

- In the tree, use arrows to move or fold and `F6` to change panels. With
  modal keys enabled, `j` / `k` move, `h` / `l` fold or enter, `/` filters,
  and `?` opens editor help.
- Press `Return` to edit, `t` to toggle a boolean, `+` / `-` to step
  a number, `r` to reset, and `y` to copy the TOML path. **Account** or
  `A` selects a signed-in account.
- Press `F8` from a non-text control to show the privacy screen, including
  from a form after tabbing out of its text field. From the settings tree,
  the default `SPC z` chord also works with modal keys enabled.
- Use `Ctrl+Return` to submit a form. In a multiline field, `Return`
  inserts a newline. The focused field keeps its native text editing keys.
- Button labels show their shortcuts. A button marked **Enter** activates
  when focused. In a collection, `a` adds, `i` enters a chat ID, `Shift+D`
  duplicates, `Shift+R` renames, `x` removes, and `Shift+J` / `Shift+K`
  reorder where supported. Plain-letter shortcuts yield to text fields.
  At narrow widths, Available chats stacks below Members and Tab reaches each
  action through vertical scrolling. In the chat picker,
  `/` focuses search and `Space` or **Add** adds the selected chat.
  Use **Enter ID** to type a chat ID.
- Use `u` to undo staged edits and `d` to review them. In **Lines**,
  `[` / `]` page through the review when the diff has focus. **Apply
  changes** is a separate action. `Esc` backs out and asks before
  discarding typed or staged changes where applicable.

See [Customize settings in the configuration guide](config.md#customize-settings-on-desktop).

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
(vector of deliberately unbound command keywords). `J` / `K` in `:chats` apply
directly in the chat list. A user `:chats` binding takes precedence there, and
a user `:normal` binding or removal takes precedence over the built-in
`:chats` default. Other `:chats` chords use the localleader. String values
inside a map label a prefix in which-key and the status line. Uppercase letters (`"G"`)
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
  palette (`SPC ,`), topic picker header (`SPC b t`), and peer-scoped commands
  in `M-x`.
- **Zen fullscreen chat (`SPC t z`, `SPC w m`):** enters OS fullscreen with the
  chat list, folder rail, and modal status bar visible. The info sidebar is
  hidden. `SPC w z` or the chat header menu toggles the chat list and folder
  rail while a conversation is open. On narrow displays it opens the full
  chat list. Resizing the list in Zen keeps the ordinary window's width
  preference. Fullscreen uses normal bubble alignment and centers dates in
  the conversation. `Esc` cancels a pending chord or layer first, returns from
  Insert to Normal next, then goes back from the chat within fullscreen.
  Toggle Zen again to restore the prior window state.
- **Privacy screen (`SPC t p`, `SPC z`):** immediately covers all open Purple
  windows (primary and separate chat windows) with a neutral clock and date
  backdrop, closes media viewers and PiP windows, hides tooltips, popups,
  already-shown notifications, and auxiliary top-level windows (call panels,
  group call windows, Instant View, and bot web apps without ending active
  calls), intercepts any newly shown auxiliary top-level surfaces (such as an
  incoming call panel, async Instant View, web app, or popup) through a single
  app-level filter while active so incoming calls remain answerable after
  dismissal, clears OS Now Playing metadata without stopping playback, suppresses
  incoming notification previews and dock / menu-bar / window-title unread
  badges, hides tray account items and disables macOS global menu actions, and
  blocks modal commands, IME/drop events, notification clicks, and `tg://` links
  from revealing a chat until dismissed (`Esc`, `q`, or Return). Dismissing
  restores hidden auxiliary windows (or defers restoring them until passcode
  unlock when locked via **Lock with passcode** or auto-lock), badges, Now
  Playing metadata, menus, and window titles, and runs any queued `tg://` start
  URLs. The privacy screen is not a security lock; when a local passcode is
  configured, it offers a separate **Lock with passcode** button that engages
  the passcode lock before dismissing the privacy overlay so chats do not flash.
  - **World clocks.** Set an ordered list under `[privacy_screen]` in Customize
    or in app-wide `settings.toml`. Clocks use IANA time zones. The cover
    hides clocks with the same current UTC offset and marks differences in
    calendar date. A label is visible on the cover.
  - **Optional weather.** The cover can show current conditions, the next six
    hours, and the next three days. Under **Settings → Purple → Privacy screen
    weather**, choose a town or postal code, resolve a street address with
    Apple on macOS, or enable approximate current location. The default is
    off. Weather source and temperature units are device settings. The cover
    displays a generic location label and suspends weather while an app proxy
    is enabled. The forecast request sends the rounded area and your IP to
    Open-Meteo. Town searches send the entered text to Open-Meteo only when
    submitted. macOS address search sends the submitted address to Apple.
- **Forum groups and threaded bots:** `SPC b t` / `, t` opens the live-updating
  topic picker for the current forum group or threaded bot. It loads more as
  you approach the end of the results or filter them. `L` loads a page and
  `g r` refreshes the list, including after a failed load. The loading indicator
  clears after ten seconds if no page arrives. An empty or duplicate page
  pauses automatic loading. `SPC b T` / `, T` opens it filtered to unread
  topics, `SPC b n`/`N` (`, n`/`N`,
  `] T`/`[ T`) cycle topics, `] U` jumps to the next unread topic, `SPC b l` /
  `, b` toggles the last viewed topic, and `SPC b h` / `, h` / `g ^` opens or
  closes the parent topic list. `SPC b c`, `, c`, and the picker's `c` action
  open the new topic or thread composer when Telegram grants permission. For
  bots that manage their own threads, the action opens the off-topic composer
  for a regular message. The bot decides whether to start a thread.
- **Purple notices and notice history (`SPC h e`):** Detailed notices,
  warnings, errors, and notices with actions open a dialog in either keyboard
  mode. Use the mouse or `Tab`, `Enter`, and `Esc`. With modal keys enabled,
  `j`/`k`/`gg`/`G` scroll, `h`/`l` select buttons, `o` selects the
  primary action, `y` copies logs, `?` opens help, and `q` closes. Close is
  selected on arrival. **Copy logs** copies the visible notice and up to 60
  recent lines for that subsystem. Before/after values in the dialog
  resolve bare chat IDs in the current account. Hidden, unknown, ambiguous,
  and negative IDs remain literal. Notices wait for the correct active
  window and a pause in typing. Background notices coalesce and also wait
  for blocking layers and pending modal keys. Short acknowledgements can
  still use the status line or a toast. `SPC h e` reopens recent
  per-account notices in a memory-only `*Messages*` list that clears on logout
  and is unavailable while passcode-locked or under the privacy screen.
  Archive removal warnings contain no account details and are shared across
  accounts, including after logout. Clearing the displayed history also
  clears its pending notices. Passcode lock, screen lock, and the privacy
  cover defer notices and acknowledgements.

- **Diagnostics in M-x.** Run `:log-export` to save a redacted bundle or
  `:log-clear` to clear recent Purple diagnostic events after confirmation.
  Both actions are also available under Purple → Diagnostics & logs.

## Known limits

- Desktop language packs can translate labels while the app layout stays
  left-to-right. Modal surfaces follow the same direction as the stock
  Desktop UI.

## Upstream hooks

- `core/application.cpp` and `.h`: delegates key press, shortcut override,
  paired key release, window deactivation, and IME events to
  `Purple::HandleModalKeys`; suppresses `unreadBadge()`, blocks `tg://` and
  separate-window navigation while `Purple::PrivacyScreenActiveAny()` is active,
  syncs newly created windows to the privacy screen, and closes media view and
  call panels when the privacy screen activates.
- `mainwindow.cpp` and `window/main_window.cpp`: attaches the modal status line
  and docked which-key drawer in `finishFirstShow()`, reserves status-bar height
  beneath `_main` in `updateControlsGeometry()`, gives full-height layers such
  as the main menu precedence over the status strip, and lays out and raises
  the privacy screen above layers and below passcode lock in `fixOrder()`.
  The window title is `Clock` while the privacy screen is active.
- `mainwidget.cpp`, `window/window_filters_menu.h`, and
  `window/window_session_controller.*`: hides the folder rail, chat list, side
  shadows when the Zen chat list is toggled off, and hides the third column
  throughout Zen. `window/window_adaptive.*` selects normal chat layout in
  fullscreen.
- `window/notifications_manager.cpp` and `media/view/media_view_overlay_widget.cpp`:
  hides notification details and blocks notification-click chat opening while the
  privacy screen is active, and closes active PiP windows when it activates.
- `core/shortcuts.h`, `dialogs/dialogs_widget.*`, `history/history_widget.*`,
  `history/history_inner_widget.*`, `history/view/history_view_*`,
  `ui/boxes/calendar_box.cpp`, `window/window_main_menu.cpp`, and
  `window/section_widget.cpp`: existing focus, visible message hint, calendar,
  main menu, and chat background hooks.

## Open chats with the keyboard

Open the chat palette with `SPC ,` or `SPC RET` to browse chats, archived
chats, channels, and contacts in their usual order. `SPC b b` starts with the
most recently viewed chats and puts the current chat last among equal matches.
Both respect Work Mode visibility. Recency stays in encrypted per-account
storage and clears on logout.

Every query word must match. Literal name matches rank first, followed by name
matches using the other keyboard layout, username or phone, then contact note.
Persian and US QWERTY layout conversion works in either direction for names.
The palettes dim and elide contact notes and highlight matching text. Use
Up or Down to move and Enter to open the selected chat.

# Modal keyboard shortcuts

Enable **Modal keyboard shortcuts** under **Settings -> Purple**. When enabled,
each main window opens in **Normal** mode with a persistent **modal status line**
docked along the bottom edge, beneath the main layout.

## Modes and status line

The status line follows the folder rail, chat list, conversation, and any info
column. It uses their colors and continues their dividers. Text follows the
composer and avatar insets, with shortcuts aligned to the composer icons.
The mode sits under the rail in the folder-label font, or at the leading edge
in windows without a rail.

- **Normal** (green label, default when enabled): keys dispatch commands.
  Switching chats, folders, or panes stays in Normal mode while clearing any
  in-flight prefix or count.
- Hold a single-key motion such as `j`, `k`, `J`, `K`, `C-d`, or `C-u`
  to repeat it. A numeric count applies to the first press. Changing focus
  or leaving Normal mode ends the motion.
- In history and topics, a tap of `j` or `k` eases one step. The first repeat
  starts continuous pixel scrolling, and release stops it with a short ease-out.
  The cheatsheet, notice bodies, and Customize review scroll the same way.
  Hold Shift during sustained `j` / `k` scrolling to double the speed.
  Release Shift to return to the regular speed without stopping.
  Repeated half-page and page motions retarget the current scroll animation.
  Lists and pickers still move one row at a time. Turning off Animations
  restores discrete scroll steps.
- **Insert** (blue label): press `i` or `a`, start a reply or edit hint, open
  search, or click directly inside the message composer to type. Ordinary
  typing, IME composition, and Telegram shortcuts work normally. Press `Esc`,
  `C-[`, or `C-g` to return to Normal mode.
- **Transient capture and layer labels:** the status line shows
  `LEADER` (indigo) after `SPC`, `LOCAL` (teal) after `,` or `SPC m`, and
  `HINTS` (purple) during
  message/URL hints, `M-x` (violet) in the command palette, `KEY?` (amber)
  during `SPC h k`, and `HELP` / `FILTER` while the cheatsheet, bindings list,
  describe card, or stock shortcuts layer is open. While a prefix, which-key
  drawer, hint capture, or layer is open, the status bar replaces the
  Normal-mode hint on its right side with that surface's active keys.
- **Pending counts and prefixes:** numeric counts (`5j`, `2C-d`) and pending
  chords (`SPC b -`, `, -`, `y -`) echo immediately on the status line.
  An idle pane shows its configured localleader as a dim cue such as
  `, local`. Pressing `DEL` inside a prefix
  backs up one stroke; `Esc` or `C-g` cancels. Unknown endings report
  `<chord> is undefined` on the status line and never fall through to Normal
  mode. Unavailable commands report what they need.
- **Unread unmuted chat counts:** the chat list segment shows positive counts
  of unread unmuted chats, broken down into direct
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
window width. The outline ends at the top corners, leaving the join with
the status line flush. Keycaps reuse the stock `Ui::KeysLegend` style.
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
- **Half-page list motion:** `Alt+Up` and `Alt+Down` move through half of the
  visible rows, or scroll half of the viewport in a scrolling panel. Lists
  move at least one row when the viewport is short. Playlist editing keeps
  its existing track-reorder shortcut.
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
    track, `, f` / `, b` seek forward/back, `, l` playlists. The library and
    the chat Music dialog both apply the current `:normal`, `:music` and
    `:localleader` bindings. The bare Music defaults for `m`, `v`, `V`,
    `n` and `N` yield to overlapping `:normal` bindings. Set a bare key
    to `nil` in `:normal` to disable its Music default.
    In `, u`, Up/Down, Page Up/Page Down and `C-j`/`C-k` move through the
    results while typing. `RET` plays the selected track or marked tracks.
    In the dialog's Normal List view, Left/Right seek the current song.
- **Window hints (`f` / `F`):** `f` labels visible chats, topics, folders,
  supported links, media, and controls. A message label starts a reply when
  replies are allowed. The pinned bar's body jumps to the pinned message.
  Its pinned-list button has its own hint. Forwarded-source and author links
  use blue navigation hints. Audio play and download controls use teal hints.
  The topmost Purple or Telegram layer owns the hints
  when a box is open. Type the label, use `DEL` to undo a letter, or press
  `Esc` / `C-g` to cancel. The composer and search fields retain normal typing
  in Insert mode. Window hints are unavailable while locked or covered.
- **Hint actions (`F`):** choose a label to see the target's actions. Press
  the displayed key or use `j` / `k` and `RET`. `RET` runs the default action
  where one is available. `DEL` goes back from a submenu, `?` explains the
  keys, and `Esc` / `C-g` closes the panel. Message menus use `y` for copy,
  `r` for reply, `d` for delete, and `m` for the full native menu when those
  actions are available. `R` opens Telegram's reaction picker from that menu.
  Telegram still shows its usual confirmations.
- **Yank (`y`) and message hints (`g`):**
  - `y y` / `y m` copy visible message text, `y l` copy message link, `y u`
    copy URL in message text.
  - `g o` opens a visible URL in plain or rich message text. In a rich
    message, expand a Details block before hinting links inside it.
  - `g s` select message, `g m` open message context menu, `g r` reply to
    message, `g e` edit message, `g u` jump to unread.
  - Hints use blue for navigation, violet for reply, teal for playback, amber
    for menus and neutral for other controls. Close has a rounded red border.
    Delete, Remove, Leave and Discard have clipped red corners. History badges
    stay in their pane and leave visible text clear. A line with an endpoint
    dot connects a displaced badge to its control. Typing a prefix underlines the letters
    already entered, outlines the matching targets and preserves their positions.
    Backspace restores the previous set. Configure marks and opacity in
    [settings.toml](config.md#hint-badges). Telegram's copy and selection
    restrictions apply.
- **Insert mode (`:insert`):** `C-[` enters Normal mode, `M-x` opens the
  command palette and restores focus to the composer on cancel.

Window hints cover Qt buttons, dialog and topic rows, shared media and Music
rows, and message elements with a click handler. Native OS dialogs, embedded
webviews, hold-to-record controls, and painted targets without a supported hit
test are outside their scope. A narrow or crowded viewport may omit a label
when there is nowhere to place it without covering another label. The full
native message menu includes live reaction and read-receipt details.

## Help views and `M-x` command palette

- **`?` (Cheatsheet) and `SPC h b` / `SPC ?` (All Active Bindings):** built on
  stock `Ui::LayerWidget` and `Ui::KeysLegend` keycaps. `?` presents six
  curated task cards with live-resolved bindings and a footer showing the
  `keys.edn` path and status. Press `TAB` (or click the header tabs) to switch
  to the exhaustive active binding list grouped by scope, with orderless
  filtering (`i`, `a`, or `/`), `j`/`k` scrolling, and `q` to close from the list.
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

With modal keys on, enter a list's filter with `i`, `a`, `/`, or a click.
A keyboard layout that types `/` with a modifier works too. In Unread, Help
and All Bindings, Notice History, and the topic picker, `Esc`, `C-[`, or
`C-g` returns from the filter to the list and keeps the query. From the
list, `Esc` clears the query and keeps the highlighted item if it is still
listed. `Esc` in an unfiltered list closes it and returns focus to its
origin. `q` closes directly. In the Customize collection picker, use `i`
or `/` to filter and `a` to add the selected chat.

M-x, Describe Command, and chat palettes open with the search field
focused. `C-n` / `C-p` move through results from search. `Esc` closes the
palette and restores focus. `F6` switches between M-x search and results
while keeping the query.
Keypad Enter runs or describes the highlighted command, just like Return.

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
  Enter the orderless filter with `i`, `a`, or `/`. The status line shows
  `INSERT` while typing and `UNREAD` in the list. `RET` opens the selected chat
  from either focus. The selection follows the chat as results narrow or move.
  If it is filtered out, the highlighted row moves to a nearby result.
  Clearing with `Esc` keeps the highlighted chat, or returns to the original
  chat if the filter had no results.
  In the list, navigate with `j`/`k`, `C-n`/`C-p`, `g g`, or `G`, refresh with
  `g r`, mark with `m` / `u` / `U` / `t` / `* d` / `* g` / `* c`, open with `o`,
  and mark read with `r` or `x`. Marking more than one
  chat as read asks for an explicit `y` / `RET` confirmation first because
  marking read sends read receipts.
- **Profile pictures in modal menus:** circular peer avatars appear beside
  people, groups, channels, and accounts in the unread buffer (`SPC u`), chat
  palette (`SPC ,`), topic picker header (`SPC b t`), and peer-scoped commands
  in `M-x`.
- **Zen fullscreen chat (`SPC t z`, `SPC w m`):** enters OS fullscreen with the
  chat list, folder rail, and modal status bar visible. The info sidebar is
  hidden. You can enter Zen from the chat list with no chat open, then open
  a conversation within fullscreen. `SPC w z` or the chat header menu toggles
  the chat list and folder rail while a conversation is open. On narrow displays it opens the full
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
  URLs. Return restores the previously focused control when it is still in
  the same unlocked window and account. Modal shortcuts resume in Normal.
  The privacy screen is not a security lock; when a local passcode is
  configured, it offers a separate **Lock with passcode** button that engages
  the passcode lock before dismissing the privacy overlay so chats do not flash.
  - **World clocks.** Set an ordered list under `[privacy_screen]` in Customize
    or in app-wide `settings.toml`. Clocks use IANA time zones. The cover
    hides clocks with the same current UTC offset and marks differences in
    calendar date. A label is visible on the cover.
  - **Weather.** The cover shows current conditions beside the clocks and a
    24-hour temperature and precipitation timeline above the next three days.
    Narrow or short covers show up to 12 hours. It uses approximate current location by default
    and waits until the cover is shown to request it. When system location
    is denied, restricted, or unavailable, the cover shows only its
    non-weather content. **Settings → Purple → Privacy screen weather** shows
    the reason and lets you choose a town or postal code, look up a street
    address with Apple on macOS, or turn weather off. Weather source and
    temperature units are device settings. The cover uses a generic location
    label. Forecasts send the rounded area to
    Open-Meteo. Town searches send the text you submit. Both requests use an
    enabled SOCKS5 or HTTP app proxy, follow Qt's system proxy settings in
    system mode, and go direct when the app proxy is off or uses MTProto or
    Web. On macOS, address lookup sends the submitted address to Apple through
    its system service, outside the app proxy.
- **Forum groups and threaded bots:** `SPC b t` / `, t` opens the live-updating
  topic picker for the current forum group or threaded bot. It loads more as
  you approach the end of the results or filter them. `L` loads a page and
  `g r` refreshes the list, including after a failed load. The loading indicator
  clears after ten seconds if no page arrives. An empty or duplicate page
  pauses automatic loading. `SPC b T` / `, T` opens it filtered to unread
  topics, `SPC b n`/`N` (`, n`/`N`,
  `] T`/`[ T`) cycle topics. `] U` jumps to the next unread topic. If more
  topics may be available, it points to the unread picker. After a paging
  error, open the picker and press `g r` to retry. `SPC b l` /
  `, b` toggles the last viewed topic, and `SPC b h` / `, h` / `g ^` opens or
  closes the parent topic list. `SPC b c`, `, c`, and the picker's `c` action
  open the new topic or thread composer when Telegram grants permission. For
  bots that manage their own threads, the action opens the off-topic composer
  for a regular message. The bot decides whether to start a thread.
- **Purple notices and notice history (`SPC h e`):** Detailed notices,
  warnings, errors, and notices with actions open a dialog in either keyboard
  mode. Use the mouse or `Tab`, `Enter`, and `Esc`. With modal keys enabled,
  `j`/`k`/`gg`/`G` scroll, `h`/`l` select buttons, `o` selects the
  primary action, `y` copies logs, `?` opens help, and `q` closes. `Esc`
  cancels a pending `g` or hides help before it closes the notice. Close is
  selected on arrival. **Copy logs** copies the visible notice and up to 60
  recent lines for that subsystem. Before/after values in the dialog
  resolve bare chat IDs in the current account. Hidden, unknown, ambiguous,
  and negative IDs remain literal. Notices wait for the correct active
  window and a pause in typing. Background notices coalesce and also wait
  for blocking layers and pending modal keys. Short acknowledgements can
  still use the status line or a toast. `SPC h e` reopens recent
  per-account notices in a memory-only `*Messages*` list that clears on logout
  and is unavailable while passcode-locked or under the privacy screen.
  Filtering keeps the selected notice in view when possible. Clearing with
  `Esc` keeps the highlighted notice, or returns to the original notice
  if the filter had no results.
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
The recent palette follows Work Mode's chat view. The all-chats palette can
also open chats hidden from that view. Both honor global hiding, including
`hide_everywhere_p`. Recency stays in encrypted per-account storage and clears
on logout.

Every query word must match. Literal name matches rank first, followed by name
matches using the other keyboard layout, username or phone, then contact note.
Persian and US QWERTY layout conversion works in either direction for names.
The palettes dim and elide contact notes and highlight matching text. Searchable
pickers select the first match when they open and when you edit the query. Use
Up or Down to move and Enter to open the selected chat. In lists with the Escape
ladder, clearing the filter from the list keeps the highlighted item if it is
still present.

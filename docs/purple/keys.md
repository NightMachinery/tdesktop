# Modal keyboard shortcuts

Enable **Modal keyboard shortcuts** under **Settings → Purple**. Each main
window starts in Insert mode. With focus in the chat list or main chat, press
`Ctrl+[` to enter Normal mode. On macOS use the physical Control key.
Press `i` or `Esc` to return to Insert. Changing focus, chats, folders, accounts, or
windows returns to Insert. Search, menus, dialogs, and locked windows use
their own controls.

In Normal mode, `SPC` means Space. A guide appears after a short pause in a
prefix. `Esc` or `Ctrl+G` cancels the prefix. Insert mode uses the regular
Telegram typing and input-method behavior.
On macOS, typing a leader key in the chat list keeps focus there in Normal
mode. An input method composition can still open search.

- `SPC c b` toggles artwork behind live chats and related history sections.
  The setting is local to this installation, starts visible, and also applies
  when modal keys are disabled. Bubble colors and saved wallpapers are kept.
  Date and service chips adjust to the flat background for readable text.
  Wallpaper choosers and standalone previews show the configured wallpaper.
- `j` or `] c` moves the chat-list cursor down. `k` or `[ c` moves it up.
  `Enter` opens the highlighted chat.
- `] f` and `[ f` select the adjacent visible folder from the main chat list.
- `/` or `SPC s s` opens search for the current view.
- `SPC o m` opens the main menu, `SPC o t` opens Settings, and `SPC o s`
  opens Saved Messages. Use Up or Down to select a main menu row and Enter
  to activate it. In the main menu, `a` shows or hides the account list.
  Opening the list highlights its first available row.
- `SPC o a` opens Archived Chats, `SPC o c` opens Contacts, and `SPC o d`
  opens Scheduled Messages in a chat that supports them.
- `SPC c m` opens the chat menu. `SPC c p` previews a chat from the chat list.
- `SPC m p` toggles playback and `SPC m s` stops it. `SPC m b` and
  `SPC m f` seek by the configured step. `[ t` and `] t` move to the
  previous or next track. These commands control the current account's
  active player.
- In the main chat, replies, topics, pinned messages, and scheduled messages,
  `g s s` labels visible messages for selection and `g s y` labels messages
  with copyable text. In pinned messages, focus the message list first. Type
  the letters on a message to select it or copy its text. Albums use Telegram's
  group behavior.
  Scrolling or changing chats dismisses the labels, as do `Esc` and `Ctrl+G`.
  Scheduled messages become eligible after Telegram confirms them.
- `g s l` copies the link to a visible message when Telegram offers a
  message link. Private channel and reply links follow the same rules as
  the context menu.
  `g s m` opens the native menu for a hinted message and returns to Insert.
  Move through the menu with Up and Down and choose an action with Enter.
- `g s u` copies a visible link from message text and `g s o` opens it.
  Type the letters on the link. Copy follows the chat's copy restrictions,
  and opening follows Telegram's usual link handling and confirmations.
- `g s r` starts a reply to a visible message, and `g s e` opens an editable
  message in Telegram's editor. Both return to Insert so you can type right
  away. Sending a reply or saving an edit uses the usual controls.
  Reply works in chats and their reply threads or topics. Edit also works
  for eligible scheduled messages.
- `SPC a 1` through `SPC a 6` switch to another signed-in account in the
  order shown in Telegram's account list.
- With the chat list and conversation visible, `SPC w d` focuses the chat list
  and `SPC w c` focuses the conversation. Focus changes return to Insert.
- `[ m` and `] m` scroll the active conversation by one visible page
  toward older or newer messages. They work in the main chat, replies,
  topics, pinned messages, and scheduled messages.
- `] ]` jumps to the newest messages in the current view, even if the last
  page needs to load. `] u` jumps to unread messages in the conversation.
  Telegram takes you to the newest messages if there is no unread position
  to jump to.
- `SPC c d` opens the history calendar in a chat or forum topic. Use
  Shift+Left or Shift+Right to move by day, Shift+Up or Shift+Down to move
  by week, and Enter to choose the highlighted date. Page Up and Page Down
  still change the month. If the new month has no highlighted day, Enter
  highlights one and a second Enter chooses it.
- `SPC ?` lists bindings. `SPC SPC` opens command search. Type a command
  name, ID, or binding, use the arrow keys to select, and press Enter.
- `SPC h k` waits for a binding and describes it. `SPC h f` searches command
  descriptions. Both leave the described action unexecuted.

Menus, settings, and compose overlays use Telegram's built-in navigation.
Alt+Space stays with the system or editor.
## Custom bindings

Create `keys.json` beside Purple's local [settings.toml](config.md), then
restart Purple. It uses the same `keys` and `command` fields as Telegram's
shortcut file, with space-separated keys for modal sequences. For example,
this moves the archive command to `SPC o r`:

```json
[
  { "keys": "SPC o a", "command": null },
  { "keys": "SPC o r", "command": "open-archived-chats" }
]
```

Use the command IDs from `SPC h f`. A command ID adds or replaces a binding.
Use `null` to remove one. Help, command search, and which-key show the
effective bindings. `SPC ?`, `SPC SPC`, `SPC h k`, and `SPC h f`
always remain available.

Each sequence can contain up to four logical keys. Use letters, digits,
`SPC`, `Enter`, brackets, slash, or a question mark after the first key.
Sequences cannot start with `i`, which returns to Insert. Key names are
case-insensitive.
Keep the file under 16 KiB and at most 128 entries. Duplicate sequences,
sequences that start another binding, and unknown command IDs cause Purple to
log a warning and use the defaults for that run. Purple keeps this file local.

## Upstream hooks

`core/application.cpp` delegates key presses after fullscreen and chat-switch
handling, as well as shortcut overrides, paired releases, window deactivation,
and IME events.
`core/shortcuts.h` exposes the existing pause-aware dispatch overload for
commands that use Telegram's native request handlers.
`dialogs/dialogs_widget.*` and `history/history_widget.*` expose eligible focus
surfaces. `history/history_inner_widget.*` exposes the existing visible
message enumeration and geometry changes. `HistoryView::ListWidget` exposes
the same seams. Hints use the existing restrictions and clipboard helpers.
The history and section selection callbacks preserve focus for hint selection.
`ui/boxes/calendar_box.cpp` routes keyboard day movement through the stock
highlight and the same single-date validation used for mouse selection.
`window/section_widget.cpp` subscribes sections to repaint changes and checks
wallpaper visibility before painting the live background.

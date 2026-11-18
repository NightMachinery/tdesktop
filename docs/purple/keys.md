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

- `SPC c b` toggles artwork behind live chats and related history sections.
  The setting is local to this installation, starts visible, and also applies
  when modal keys are disabled. Bubble colors and saved wallpapers are kept.
  Wallpaper choosers and standalone previews show the configured wallpaper.
- `j` or `] c` moves the chat-list cursor down. `k` or `[ c` moves it up.
  `Enter` opens the highlighted chat.
- `] f` and `[ f` select the adjacent visible folder from the main chat list.
- `/` or `SPC s s` opens search for the current view.
- `SPC o m` opens the main menu, `SPC o t` opens Settings, and `SPC o s`
  opens Saved Messages. Use Up or Down to select a main menu row and Enter
  to activate it.
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
  `g s m` opens the native menu for a hinted message. Use the menu to reply,
  forward, or copy a message link when those actions are available.
- `SPC a 1` through `SPC a 6` switch to another signed-in account in the
  order shown in Telegram's account list.
- With the chat list and conversation visible, `SPC w d` focuses the chat list
  and `SPC w c` focuses the conversation. Focus changes return to Insert.
- `SPC ?` lists bindings. `SPC SPC` opens command search. Type a command
  name, ID, or binding, use the arrow keys to select, and press Enter.
- `SPC h k` waits for a binding and describes it. `SPC h f` searches command
  descriptions. Both leave the described action unexecuted.

Menus, settings, and compose overlays use Telegram's built-in navigation.
Alt+Space stays with the system or editor.
Bindings use Qt's logical keys. A future local binding file could map chords
to registered command IDs without evaluating code.

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
`window/section_widget.cpp` subscribes sections to repaint changes and checks
wallpaper visibility before painting the live background.

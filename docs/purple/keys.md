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
- In the main chat, `g s s` labels visible messages for selection and
  `g s y` labels messages with copyable text. Type the letters on a message
  to select it or copy its text. Albums use Telegram's group behavior.
  Scrolling or changing chats dismisses the labels, as do `Esc` and `Ctrl+G`.
- `SPC ?` lists bindings. `SPC SPC` opens command search. Type a command
  name, ID, or binding, use the arrow keys to select, and press Enter.
- `SPC h k` waits for a binding and describes it. `SPC h f` searches command
  descriptions. Both leave the described action unexecuted.

Menus, settings, and chat sections with their own compose controls still use
Telegram's built-in navigation. Alt+Space stays with the system or editor.
Bindings use Qt's logical keys. A future local binding file could map chords
to registered command IDs without evaluating code.

## Upstream hooks

`core/application.cpp` delegates key presses after fullscreen and chat-switch
handling, as well as shortcut overrides, paired releases, window deactivation,
and IME events.
`dialogs/dialogs_widget.*` and `history/history_widget.*` expose eligible focus
surfaces. `history/history_inner_widget.*` exposes the existing visible
message enumeration and geometry changes. `HistoryWidget::updateTopBarSelection`
can preserve focus after keyboard selection. `window/section_widget.cpp` subscribes sections to repaint changes
and checks wallpaper visibility before painting the live background.

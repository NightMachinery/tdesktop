# Modal keyboard shortcuts

Enable **Modal keyboard shortcuts** under **Settings → Purple**. Each main
window starts in Insert mode. With focus in the chat list or main chat, press
`Ctrl+[` to enter Normal mode. On macOS use the physical Control key.
Press `i` or `Esc` to return to Insert. These are the default keys. Changing focus, chats, folders, accounts, or
windows returns to Insert. Search, menus, dialogs, and locked windows use
their own controls.

In Normal mode, `SPC` means Space. A guide appears after a short pause in a
prefix. `Esc` or `Ctrl+G` cancels the prefix. Insert mode uses the regular
Telegram typing and input-method behavior.
On macOS, typing a leader key in the chat list keeps focus there in Normal
mode. An input method composition can still open search.
With a non-ASCII keyboard layout such as Persian, press the U.S. key positions
for `Ctrl+[` and Normal mode chords. ASCII layouts use their own key labels.
Insert mode continues to use the selected input source for text.

- `SPC c b` toggles artwork behind live chats and related history sections.
  The setting is local to this installation, starts visible, and also applies
  when modal keys are disabled. Bubble colors and saved wallpapers are kept.
  Date and service chips adjust to the flat background for readable text.
  Wallpaper choosers and standalone previews show the configured wallpaper.
- `j` or `] c` moves the chat-list cursor down. `k` or `[ c` moves it up.
  `Enter` opens the highlighted chat.
- `] f` and `[ f` select the adjacent visible folder from the main chat list.
- `/` or `SPC s s` opens search for the current view.
- `SPC o m` opens the main menu, `SPC o s e` opens Settings, and
  `SPC o s a` opens Saved Messages. Use Up or Down to select a main menu row and Enter
  to activate it. In the main menu, `a` shows or hides the account list.
  Opening the list highlights its first available row.
- `SPC o a` opens Archived Chats, `SPC o c` opens Contacts, `SPC o p`
  opens Calls, and `SPC o d` opens Telegram's Downloads view.
- `SPC n g` opens New Group, `SPC n c` opens New Channel, and `SPC n p`
  opens Add Contact. Use the native dialog to enter details and confirm.
- `SPC c m` opens the chat menu. `SPC c p` previews a chat from the chat list.
- `SPC c i` opens information about the conversation or forum topic.
  Use `SPC c h p` for pinned messages, `SPC c h s` for scheduled messages,
  and `SPC c h d` for the calendar where those views are available.
- `SPC c s` leads to shared media. Use `a` for audio, `f` for files, `l` for
  links, `v` for photos and videos, `r` for voice and round messages,
  `g` for GIFs, and `p` for polls.
- `SPC c u` opens Telegram's file picker when the attachment button is
  available in the current chat, reply, topic, or scheduled-message view.
  Telegram handles file selection and confirmation.
- `SPC m m` toggles the Music library and `SPC m l` opens Playlists for the
  active account.
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
- With the chat list and conversation visible, `SPC w l` focuses the chat list
  and `SPC w c` focuses the conversation. Focus changes return to Insert.
- `[ m` and `] m` scroll the active conversation by one visible page
  toward older or newer messages. They work in the main chat, replies,
  topics, pinned messages, and scheduled messages.
- `] ]` jumps to the newest messages in the current view, even if the last
  page needs to load. `] u` jumps to unread messages in the conversation.
  Telegram takes you to the newest messages if there is no unread position
  to jump to.
- `SPC c h d` opens the history calendar in a chat or forum topic. Use
  Shift+Left or Shift+Right to move by day, Shift+Up or Shift+Down to move
  by week, and Enter to choose the highlighted date. Page Up and Page Down
  still change the month. If the new month has no highlighted day, Enter
  highlights one and a second Enter chooses it.
- `SPC ?` lists bindings. `SPC SPC` opens command search. Type a command
  name, ID, or binding, use the arrow keys to select, and press Enter.
  While you compose text with an input method, Up and Down stay with the editor.
- `SPC h k` waits for a binding and describes it. `SPC h f` searches command
  descriptions. Both leave the described action unexecuted.

Menus, settings, and compose overlays use Telegram's built-in navigation.
By default, Alt+Space stays with the system or editor.
## Custom bindings

Purple ships the [default keymap](../../Telegram/Resources/purple/default-keys.edn)
as a bundled EDN resource. Use it to look up the default chords and
command names for each mode. Your local file can contain just the
bindings you want to change.

Save `keys.edn` beside Purple's local [settings.toml](config.md) and restart
Purple. Group bindings with `:normal` and `:insert`. Each chord maps to a
command keyword from `SPC h f`. For example:

```clojure
{:normal {"C-S-j" :chat-cursor-next
          "SPC o a" nil}
 :insert {"C-S-j" :chat-palette
          ;; Press Option+Space, then m.
          "M-SPC m" :open-music}}
```

The maps overlay each mode's defaults. Use `nil` to remove a default.
A chord may name different commands in different modes. Insert bindings start
with Ctrl, Alt, or Super, so ordinary typing stays in the editor. For an
Insert prefix such as `M-SPC`, a matching next key completes the chord and
an unmatched key goes back to the editor. Escape cancels a pending chord.
Telegram's registered shortcuts take precedence over configured modifier
chords. Bindings apply only in chat views, the chat list, and eligible Music
focus, with Telegram's dialogs and input-method composition keeping their
own keys.

Write consecutive strokes with spaces. Use Doom's `C-` (Control), `M-`
(Alt or Option), `S-` (Shift), and `s-` (Super or Command). The longer
`Ctrl+`, `Alt+`, `Shift+`, and `Super+` spellings also work.
A stroke may use letters, digits, ASCII punctuation, arrows,
Home, End, PageUp, PageDown, function keys, `SPC`, or `Enter`/`RET`.
For punctuation produced with Shift, write the resulting symbol, such as
`?` or `C-+`.
Chords have at most four strokes. `enter-normal-mode` and `enter-insert-mode`
are commands too. Keep an Insert binding for `enter-normal-mode`. Escape
always exits Normal mode when no chord is pending.

The built-in guide and command search show effective bindings, including
Insert bindings. `SPC ?`, `SPC SPC`, `SPC h k`, and `SPC h f` stay available
in Normal mode. The file limit is 16 KiB and 128 entries. Unknown modes,
command IDs, duplicate normalized chords, or conflicting prefixes reject the
file and use the defaults. Reuse a chord freely across different modes.

If `keys.edn` is absent, existing `keys.json` files keep the older Normal
defaults and their current overrides. Add a `keys.edn` file to adopt the new
defaults and add an Insert map. Purple keeps the files local.

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
The history and compose headers let Purple invoke their native file pickers
when the corresponding attachment controls are available.
`window/section_widget.cpp` subscribes sections to repaint changes and checks
wallpaper visibility before painting the live background.

## Open chats with the keyboard

`SPC Enter` opens the chat palette in Normal mode. Search your chats, archived
chats, channels, and contacts by name, username, phone number, or contact note.
Use Up or Down to move and Enter to open the highlighted chat. The palette
uses exact, case-insensitive substrings and accepts terms in any order.
A complete `@username` or international `+phone` can also resolve on Telegram.
Ordinary searches, including contact notes, are matched locally.

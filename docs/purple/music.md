# Music on Desktop

Open **Music** from the main menu and use **Music source folder** to select one
Telegram chat folder for the active account. Search by title, performer, or
filename. Each search term matches a contiguous substring, case-insensitively,
and terms can appear in any order. The newest posts appear first. Open a track to play it in the
regular player. **Load more tracks** reads the next catalog page.
You can also set the source under **Settings → Purple → Music source folder**.
In modal Normal mode, use `SPC p m` or run `open-music` in the command palette
to toggle Music. The search field opens unfocused in Normal. Press `/` or
`i`, or click the field, to enter Insert and search. Escape clears the filter
and returns focus to the track cursor. In Normal, `j` and `k` move by one row,
`Ctrl+d` and `Ctrl+u` move by half a page, and `gg` and `G` move to the first
and last matching tracks. Press `m` to mark a track. Use `v` to start or finish
a range and `V` to mark the visible rows. Return plays the cursor or the marked
tracks. In the filter, Return accepts the query and focuses the results. Escape
first cancels a range or clears marks, then returns to the prior view.
The chat list is hidden in Music by default. **Settings → Purple → Hide chat
list in Music** restores the split view. Music temporarily hides the right
sidebar. When you leave Music, the previous sidebar returns if the window
still has room for it. The toolbar opens Playlists directly.
The overflow menu contains the source picker and refresh.
`tdesktop-cli music` opens the same screen through Local automation.
Opening Music again keeps your place in the results for the same source.

The first scan backfills the selected chats in pages. Music ignores Work Mode
presets and includes every chat in the selected folder (while keeping passcode
locking and protected-content rules intact). Folders that only list explicit
chats start indexing immediately without waiting for the full main or archived
dialog lists; category-based folders request any unloaded main or archived
dialogs first. The screen displays saved results during the scan and always
explains why the list is loading or empty, including unloaded folders or dialog
lists with counts, offline state, server search or local index errors, skipped
audio messages, or a search filter that hides indexed tracks. Once you focus,
scroll, or select in the list, indexing leaves the displayed rows in place.
Search again or use **Refresh music** to see the latest results. Refresh also
checks for new tracks and resumes unfinished history. It keeps your picks while
the source folder and its chats stay the same. One paced scan runs per account.
If Telegram asks it to wait, Music displays the remaining time, then resumes
the same page. Temporary server errors retry with increasing delays and jitter.
A chat the server cannot search is skipped and its cached tracks stay available.
Both the older-history cursor and the newer-track cursor survive a restart.

After the first scan, Purple also indexes eligible new tracks it receives
from the configured folder while the Music screen is closed. Opening Music
again checks for tracks that arrived while the app was offline. New arrivals
leave the history-scan cursor in place so the next scan can still fill gaps.

The database stores track metadata, source message IDs, and scan cursors in
the local Purple settings directory. Each Telegram account and environment
has its own database. The scan never downloads audio. When you open Music
with a new source folder, it clears the old index. When a chat leaves the
selected folder, its entries are removed. Music ignores Work Mode hiding, and
playback rechecks the account, folder membership, and source message before
opening a track.

Click a track to play it, or click the checkbox to mark it. Command-click on
macOS and Ctrl-click on other platforms also mark tracks. Playing, paused,
and loading cues follow the player. The row tint and checkmark show marks,
and a border shows the keyboard cursor. Hold an arrow to move repeatedly.
While an input method is composing text, Down stays in search.

The **View** button or `, t` toggles **List** and **Tiles**. The library and
chat music dialog share the saved view. In Tiles, Left and Right or `h` and
`l` move horizontally. Both views use document thumbnails when available and
draw a cover from the track and artist names while art loads. The now-playing
panel shows the current song and offers a seek slider.

Marks reveal a bar with playback, queue, playlist, forwarding, and clear
actions. Narrow windows put secondary actions in **More**. The bar counts
hidden marks so a filtered selection can be reviewed before taking action.
Playing multiple marks creates or reuses a Hyper playlist in displayed order.
**Queue** appends the chosen tracks to the playback queue.

Right-click a track and choose **Copy Org link**, or focus the track and
press the system Copy shortcut. This copies a `purpleaudio:` Org link
without starting playback. It uses the same account-local bookmarks that
Doom's Telegram music link command creates. It stores no audio bytes.

Open **Playlists** from the main menu to create folders or move playlists
between them. Use Tab and Shift+Tab to focus playlist and folder actions, then
press Enter or Space to activate the focused action. Removing a folder places
its playlists back at the top level.
Hyper holds automatically created playlists. Folder names and placement are
saved for each account on this Desktop installation.

Stock shared-media search cancels a delayed query when a cached query takes
over. Clearing the filter restores the unfiltered list even if a search was
still queued.

In a chat or topic, `, u` opens **Music shared in this chat**. It uses
Telegram's shared-media history and also works in chats outside the Music
source folder. The filter matches title, artist, or filename in the loaded
tracks. **Load more tracks** expands the history and keeps the filter applied.
Use the arrows or `j` and `k` to move the cursor, and Alt+Up or Alt+Down to move
by half a page. Press Escape to close the filter, then Escape to close the
dialog. The **Actions** button or a right-click offers play, forward, go to
message, add to playlist, and copy message link. The library's **More** menu and
localleader bindings expose the same track actions.

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
`Ctrl+d` and `Ctrl+u` move by half a page, and `gg` and `G` move to the ends
of the loaded rows. Press `m` to mark a track and Return to play. Escape
clears marks or returns to the prior view.
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

Click a track to play it. Use Command-click on macOS or Ctrl-click on
other platforms to mark tracks. Playing, paused and loading states follow the
player. The row tint shows marks, and a border shows the keyboard cursor.
Press Down from search to focus the current
result and Escape from the results to return to search. While an input method
is composing text, Down stays in search. In the list, the arrow keys move the
cursor and Space toggles selection. Press Enter or use
the playback button to play the selection. Two or more tracks create a
playlist in Hyper, in the order you selected them. Repeating the same
selection reuses the playlist while the saved sources still match.

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
message, add to playlist, and copy message link.

# Music on Desktop

Open **Music** from the main menu and use **Music source folder** to select one
Telegram chat folder for the active account. Search by title, performer, or
filename. The newest posts appear first. Open a track to play it in the
regular player. **Load more tracks** reads the next catalog page.
You can also set the source under **Settings → Purple → Music source folder**.
In modal Normal mode, use `SPC m m` or run `open-music` in the command palette.
`tdesktop-cli music` opens the same screen through Local automation.

The first scan backfills the selected chats in pages. The screen displays
saved results during the scan. Once you focus, scroll, or select in the list,
indexing leaves the displayed rows in place. Search again or use
**Refresh music** to see the latest results. Refresh also checks for new
tracks and resumes unfinished history. It keeps your picks while the source
folder and its visible chats stay the same. A failed chat search leaves its
previous results intact and reports that the scan is incomplete.

After the first scan, Purple also indexes eligible new tracks it receives
from the configured folder while the Music screen is closed. Opening Music
again checks for tracks that arrived while the app was offline. New arrivals
leave the history-scan cursor in place so the next scan can still fill gaps.

The database stores track metadata, source message IDs, and scan cursors in
the local Purple settings directory. Each Telegram account and environment
has its own database. The scan never downloads audio. When you open Music
with a new source folder, it clears the old index. When a chat leaves the
selected folder, its entries are removed. Search filters out chats hidden by Purple, and playback
rechecks the account, chat, and source message before opening a track.

Double-click a track to play it. Use Command-click on macOS or Ctrl-click on
other platforms to select tracks. With the track list focused, the arrow keys
move the cursor and Space toggles selection. Press Enter or use the playback
button to play the selection. Two or more tracks create a playlist in Hyper,
in the order you selected them. Repeating the same selection reuses the
playlist while the saved sources still match.

Right-click a track and choose **Copy Org link**, or focus the track and
press the system Copy shortcut. This copies a `purpleaudio:` Org link
without starting playback. It uses the same account-local bookmarks that
Doom's Telegram music link command creates. It stores no audio bytes.

Open **Playlists** from the main menu to create folders or move playlists
between them. Removing a folder places its playlists back at the top level.
Hyper holds automatically created playlists. Folder names and placement are
saved for each account on this Desktop installation.

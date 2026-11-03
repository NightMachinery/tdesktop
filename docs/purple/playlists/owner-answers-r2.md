# the owner's playlist answers, round 2 (2026-09-30, about 16:45 CEST)

Builds on playlist-decisions-r1.md.

- P3: (d). Two levels (Delete to Recently deleted, Delete forever purges) plus an automatic purge after 30 days in Recently deleted.
- P4: automatic sync after edits, coalesced (60 s quiet timer, 10 min maximum wait), plus a check for other devices' changes on app start or opening Playlists at most every 15 min; manual Sync now stays. Automatic sync must be a configurable Purple option.
- P9: allow sync chats that have other readers, but warn when the chosen chat has other members, and warn when someone new joins it. the owner's design (speech to text, paraphrased): create a private "Purple sync" channel and use it as the default sync chat. A new Purple session finds that channel and reads the records there. If the user switches the sync chat to another chat, the Purple channel keeps a short redirect record pointing to the new chat. If the channel does not exist, Purple asks permission to create it; if Telegram's limit on channels is reached, use Telegram's normal limit machinery (the dialog that offers to leave chats). the owner asked for my opinion.
- P10: apply the proposal: per-account sections inside settings.toml that override global keys for that account; settings sync keeps carrying the one file through the home account.
- P15: asked for performance suggestions, and whether the merge cost happens on every sync.
- P18: if playlist sync is off at logout, warn and offer a final sync first or deleting without one.
- Everything else from round 1 stands as answered or defaulted.
- New: sharing playlists with other users. Forward all songs of the playlist with sender and caption hidden (works for non-Purple users), and also send a file that loads the forwarded songs as a playlist in Purple, updating an earlier import with the same id. Open: a unique id per playlist; replace or merge on re-share, since the recipient may have edited it.

Round 2b (about 16:58):
- Sync chat discovery: mark the chosen chat (any chat) with a tag and search for it on startup; the default is still a Purple-created private channel. Supersedes the redirect record unless the tag cannot work. Coordinator's suggested tag #purplesyncchat, pending the design's tokenization check.
- Protected songs when sharing: re-upload a downloaded copy, behind a confirmation dialog.

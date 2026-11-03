# the owner's playlist answers, round 1 (2026-09-30, about 15:45 CEST)

Decided:
- P5: (c), also Add to playlist in the chat and shared-media multi-select bars.
- P6: (b) and (c): Download all per playlist, and a Keep downloaded switch; default keep forever.
- P8: (b), a navigation drawer item, with a Purple setting to show it, default shown.
- P11: relink, and keep earlier links in the record as fallbacks, so a deleted source falls back to another.
- P13: a new Purple setting, on by default, allowing all actions in protected chats; songs from them fully usable in playlists (user-control design, workflow wf_7ced1779-942).
- P15: relax the cap, and gzip the record.
- P18: when logging out with playlist sync on, send a last sync, then delete the local files (sync off: not stated; proposed keep, awaiting the owner).
- P19: (b), recently used first.
- P20: (b), a Purple switch, default private listening (no messages.reportMusicListen).
- P21: (b), on by default.
- Shuffle play: already in the design (Play and Shuffle buttons; global repeat and order modes).
- Before any playlist code: refactor Purple code out of upstream files (rule in both AGENTS.md: tdesktop 91b54fe2b1, Android cb6d43f92; audit workflow wf_31350ea2-5fb).

Asked for more explanation (decisions pending): P3 (deletion; when a purge leaves the cloud), P4 ("a but with a coalesce and buffer wait"), P9 (cover rule; a user-chosen sync chat instead of Saved Messages, stored in settings), P10 (why one home account; per-account settings), P15 (why 4 MiB), P20 (what the report is).

New requests: user-control settings, default on (protected chats, kept deleted messages, edit history, kept view-once and timer media, ignored restrictions including copyright-tagged files, private listening); a user-chosen sync chat.

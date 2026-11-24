# Per-chat translation exclusion

Purple adds **Don't Translate This Chat** so a chat can be excluded from
automatic translation, the translate bar/offers, and language tracking on
Desktop.

## Behaviour

When a chat is excluded:

- the chat translate bar stays hidden and any active whole-chat translation or
  pending translation offer is cleared immediately;
- language recognition and auto-translation (`TranslateTracker`,
  `History::translateOfferFrom`, and `Purple::UpdateTranslationOffer`) stop
  running for that chat;
- explicit manual **Translate** / **Translate Selected** actions on a message,
  poll, admin-log entry, or profile bio continue to work.

Exclusion is reversible from two places:

- the chat's top-bar menu shows **Don't Translate This Chat** while translation
  is enabled and switches to **Allow Translation for This Chat** once excluded;
- **Settings → Language → Don't Translate Chats** shows the count of excluded
  chats and opens the stock chat picker (`PeerListBox`) pre-checked with the
  excluded chats so chats can be added or removed.

The chat translate bar's overflow menu also offers **Don't Translate This Chat**
with an **Undo** toast.

## Persistence and sync

Excluded chat IDs are stored per account in local Desktop preferences under
`purple-translate-excluded-chats` (matching Birthday Reminders' per-account
preference pattern). Legacy basic groups and their migrated supergroups are
canonicalized so excluding or re-enabling either side keeps both consistent.

Toggling exclusion also calls Telegram's existing
`PeerData::saveTranslationDisabled(bool)` (`MTPmessages_TogglePeerTranslations`),
so the server-backed chat translate-bar suppression syncs across Telegram
clients automatically. Purple's per-chat exclusion list is kept in per-account
local preferences rather than `settings.toml` so it does not require a
`purple_core` schema bump while still reusing Telegram's own per-chat
translation-disabled sync flag.

## Upstream hooks

- `Telegram/SourceFiles/history/view/history_view_translate_bar.cpp`: watches
  `Purple::ChatTranslationExcludedChanges`, gates bar visibility on
  `Purple::ChatTranslationDisabled`, and adds the **Don't Translate This Chat**
  action to the bar menu via `Purple::AddTranslateBarExcludeAction`.
- `Telegram/SourceFiles/history/view/history_view_translate_tracker.cpp`:
  includes `!Purple::ChatTranslationExcludedValue(peer)` in `_trackingLanguage`
  so recognition stops and reverts when excluded and resumes when allowed.
- `Telegram/SourceFiles/history/history.cpp`: guards `translateOfferFrom` with
  `Purple::ChatTranslationExcluded(peer)` so auto-translation cannot activate.
- `Telegram/SourceFiles/window/window_peer_menu.cpp`: calls
  `Purple::AddPeerTranslateMenuAction` at the start of `Filler::addTranslate`.
- `Telegram/SourceFiles/boxes/language_box.cpp`: calls
  `Purple::AddExcludedTranslationChatsRow` in `LanguageBox::setupTop`.

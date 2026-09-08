/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class DocumentData;
class HistoryItem;

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class PopupMenu;
class Show;
} // namespace Ui

// Moving settings.toml between installs, with Saved Messages as the transport
// and as the version history. The whole feature is two user actions - post the
// file, import a posted one - because a chat that syncs to every device the
// account is signed in to already is a sync channel, and the fork does not have
// to become one. See docs/purple/sync.md.
namespace Purple {

// Uploads the current settings.toml to the account's own Saved Messages, named
// settings.toml and captioned with the schema version, the local time and the
// platform. Confirms first, because it puts the ids and names of every chat the
// file mentions into a message.
void SendSettingsToSavedMessages(
	not_null<Main::Session*> session,
	std::shared_ptr<Ui::Show> show);

// Posts these bytes to the account's own Saved Messages as settings.toml, with
// no confirmation box - this is what the automatic send calls, and the
// confirmation for that was given once, in words, when the switch was turned
// on. The manual action above still asks.
//
// False when there is no signed-in session to post into. That is a reason to
// try again after the next write rather than an error worth a message: the app
// can be up, and the file editable, before anyone has signed in.
[[nodiscard]] bool Upload(const QByteArray &content, int version);

// The "Import Purple settings" row on a message's context menu. Offered only
// for a document called settings.toml sitting in Saved Messages: anywhere else
// it would be someone else's file, and the point of the feature is that the
// only person who can post into that chat is you.
//
// Does nothing when the item is not that, so a caller does not have to ask.
void AddImportSettingsAction(
	not_null<Ui::PopupMenu*> menu,
	HistoryItem *item,
	not_null<DocumentData*> document,
	std::shared_ptr<Ui::Show> show);

} // namespace Purple

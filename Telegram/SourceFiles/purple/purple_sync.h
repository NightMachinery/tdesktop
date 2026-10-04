/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_sync_inventory.h"

#include <QtCore/QFile>
#include <QtCore/QRect>
#include <QtCore/QSaveFile>

#include <memory>
#include <vector>

class DocumentData;
class HistoryItem;
struct FilePrepareResult;
struct FullMsgId;
struct GlobalMsgId;
class QWidget;

namespace HistoryView {
class Element;
struct TextState;
enum class PointState : char;
} // namespace HistoryView

namespace Info {
class AbstractController;
} // namespace Info

namespace Overview::Layout {
class ItemBase;
} // namespace Overview::Layout

namespace Window {
class SessionController;
} // namespace Window

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
// Posts only into the first account of the account switcher's list, and only
// while a window showing that account is in front (the app's last active
// window); see JudgeAutoSendTarget() below. False, with a log line saying
// why, when nothing was posted: no account is signed in, or the window in
// front shows another account or none. That is a reason to try again after
// the next write rather than an error worth a message.
[[nodiscard]] bool Upload(
	const QByteArray &content,
	int version,
	Fn<void(std::optional<MsgId>)> finished);

struct ImportSettingsTarget;

struct ImportSettingsHit {
	std::shared_ptr<const ImportSettingsTarget> target;

	[[nodiscard]] bool valid() const {
		return target != nullptr;
	}
	[[nodiscard]] bool keyboard() const;
	[[nodiscard]] FullMsgId containerId() const;
};

struct ImportSettingsViewState {
	QWidget *receiver = nullptr;
	QRect viewport;
	QRect geometry;
	bool ready = false;
};

void CaptureImportSettingsHit(
	ImportSettingsHit *hit,
	not_null<Window::SessionController*> controller,
	HistoryView::Element *view,
	const HistoryView::TextState &state,
	HistoryView::PointState pointState,
	QPoint rawPoint,
	const ImportSettingsViewState &native,
	bool contentOwned);
void PrepareImportSettingsKeyboardHit(
	not_null<ImportSettingsHit*> hit,
	not_null<Window::SessionController*> controller,
	HistoryView::Element *view,
	HistoryItem *focused,
	const ImportSettingsViewState &native);
void CaptureImportSettingsFileHit(
	ImportSettingsHit *hit,
	not_null<Info::AbstractController*> controller,
	Overview::Layout::ItemBase *layout,
	HistoryItem *row,
	GlobalMsgId globalId,
	const HistoryView::TextState &state,
	QPoint rawPoint,
	const ImportSettingsViewState &native,
	bool exact);
void AddImportSettingsAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<ImportSettingsHit*> hit,
	not_null<Window::SessionController*> controller,
	HistoryView::Element *container,
	HistoryItem *focused,
	const ImportSettingsViewState &native);
void AddImportSettingsFileAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<ImportSettingsHit*> hit,
	not_null<Info::AbstractController*> controller,
	Overview::Layout::ItemBase *layout,
	HistoryItem *row,
	GlobalMsgId globalId,
	const ImportSettingsViewState &native);

void OfferNewerSettingsFromSavedMessages(
	not_null<Main::Session*> session,
	std::shared_ptr<Ui::Show> show);

[[nodiscard]] inline bool IsSyncRecordFileName(const QString &name) {
	return (name == SyncSettingsRecordFileName())
		|| (name == SyncPlaylistsRecordFileName());
}

[[nodiscard]] bool IsPurplePost(const FilePrepareResult &file);

enum class ImportOfferVerdict {
	Offer,
	InactiveAccount,
	AlreadyOffered,
	NotNewer,
};

[[nodiscard]] inline ImportOfferVerdict JudgeImportOffer(
		bool accountActive,
		int64 messageId,
		int64 lastOfferedId,
		int64 messageDate,
		int64 localStamp) {
	if (!accountActive) {
		return ImportOfferVerdict::InactiveAccount;
	} else if (messageId <= lastOfferedId) {
		return ImportOfferVerdict::AlreadyOffered;
	} else if (messageDate <= localStamp) {
		return ImportOfferVerdict::NotNewer;
	}
	return ImportOfferVerdict::Offer;
}

enum class AutoSendTarget {
	Post,
	NoAccount,
	NotFirstAccount,
};

template <typename Account>
[[nodiscard]] AutoSendTarget JudgeAutoSendTarget(
		const std::vector<Account> &signedInInSwitcherOrder,
		const Account &active) {
	if (signedInInSwitcherOrder.empty()) {
		return AutoSendTarget::NoAccount;
	} else if (signedInInSwitcherOrder.front() != active) {
		return AutoSendTarget::NotFirstAccount;
	}
	return AutoSendTarget::Post;
}

[[nodiscard]] inline QString ImportAccountLabel(
		const QString &name,
		const QString &username) {
	const auto trimmed = name.trimmed();
	if (username.isEmpty()) {
		return trimmed;
	} else if (trimmed.isEmpty()) {
		return u"@"_q + username;
	}
	return u"%1 (@%2)"_q.arg(trimmed, username);
}

[[nodiscard]] inline QString ImportWindowSwitchedText(const QString &account) {
	return u"Nothing was imported, because the window that asked was closed "
		"or now shows another account. The settings file is in the Saved "
		"Messages of %1: import it from a window that shows that account."_q
		.arg(account);
}

[[nodiscard]] inline QString SettingsImportBackupPath(const QString &path) {
	return path + u".import.bak"_q;
}

[[nodiscard]] inline QString SettingsBackupPath(const QString &path) {
	return path + u".bak"_q;
}

[[nodiscard]] inline bool WriteSettingsCopy(
		const QString &path,
		const QByteArray &bytes) {
	auto file = QSaveFile(path);
	return file.open(QIODevice::WriteOnly)
		&& (file.write(bytes) == bytes.size())
		&& file.commit();
}

[[nodiscard]] inline QString WriteImportedSettings(
		const QString &path,
		Fn<bool()> write) {
	auto current = QFile(path);
	if (current.exists()) {
		const auto importBackup = SettingsImportBackupPath(path);
		if (!current.open(QIODevice::ReadOnly)) {
			return importBackup;
		}
		const auto bytes = current.readAll();
		if (current.error() != QFileDevice::NoError) {
			return importBackup;
		}
		current.close();
		const auto backups = {
			importBackup,
			SettingsBackupPath(path),
		};
		for (const auto &backup : backups) {
			if (!WriteSettingsCopy(backup, bytes)) {
				return backup;
			}
		}
	}
	return write() ? QString() : path;
}

} // namespace Purple

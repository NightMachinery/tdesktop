/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_config.h"
#include "purple/purple_sync_local_store.h"

#include <QtCore/QStringList>

namespace Purple {

struct DesktopSyncStoreDiscovery {
	std::vector<SyncRecoveryStore> stores;
	std::map<QString, QByteArray> identities;
	QStringList accountDirectories;
};

[[nodiscard]] std::optional<DesktopSyncStoreDiscovery> DiscoverDesktopSyncStores(const QString &root);

struct DesktopSyncRecovery {
	AccountSettingsOpenTarget opening;
	SyncAccountRecoveryInput input;
	SyncAccountRecoveryPlan plan;
	std::map<QString, QByteArray> identities;
	QStringList accountDirectories;
	bool journalPresent = false;
	QByteArray journal;
};

[[nodiscard]] std::optional<DesktopSyncRecovery> InspectSyncAccountRecovery(
	Main::Session &session,
	const SyncAccountInventoryResult *inventory = nullptr);
[[nodiscard]] bool RelinkSyncAccount(
	Main::Session &session,
	const DesktopSyncRecovery &review,
	bool confirmed);

struct SyncArchiveResult {
	bool ready = false;
	bool partial = false;
	QString archive;
};

[[nodiscard]] SyncArchiveResult ArchiveSyncAccountState(
	const QString &root,
	const AccountFiles &files,
	const SyncStartAgainPlan &plan,
	const std::function<bool()> &current);
[[nodiscard]] std::optional<SyncStartAgainPlan> ReadSyncArchiveJournal(
	const QString &root,
	const AccountFiles &files,
	const AccountKey &account);
[[nodiscard]] SyncArchiveResult StartAgainSyncAccount(
	Main::Session &session,
	const DesktopSyncRecovery &review,
	bool confirmed,
	bool operationInFlight);

}

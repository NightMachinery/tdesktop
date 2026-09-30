/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_sync_config_describe.h"
#include "purple/purple_sync_config_flow.h"
#include "purple/purple_sync_inventory.h"

#include <QtCore/QDateTime>
#include <QtCore/QString>

namespace Main {
class Account;
class Session;
}

namespace Purple {

[[nodiscard]] QString SyncDeviceName(const SyncDeviceNameParts &parts);
[[nodiscard]] QString SyncDeviceName(
	const QString &platform,
	const QString &install);
[[nodiscard]] QString SyncDeviceName(const SyncConfigHeadRecord &record);
[[nodiscard]] QString SyncMomentText(const QDateTime &when);
[[nodiscard]] QString SyncWriterPlatform();
[[nodiscard]] QString SyncWriterApp();
[[nodiscard]] SyncConfigWriter SyncWriter();

[[nodiscard]] SyncSettingsFile ReadSyncSettingsFile(const QString &path);
[[nodiscard]] SyncConfigReview ReviewSyncConfig(
	Main::Account &account,
	Main::Session &session,
	const SyncAccountInventoryResult &inventory);

} // namespace Purple

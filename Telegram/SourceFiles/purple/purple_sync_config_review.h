/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_account_config.h"
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

[[nodiscard]] SyncSettingsFile ReadSyncSettingsFile(const AccountFiles &files);

enum class SyncFirstUseNeed {
	None,
	NoState,
	OtherSpace,
	Unjoined,
};

// `sync/first-use.json`, kept while no state settles first use: the answer, and the space a join entered.
struct SyncFirstUseMark {
	SyncFirstUseChoice choice = SyncFirstUseChoice::None;
	QString joinSpace;
	int64_t checkedAt = 0;
	QString fingerprint;

	friend bool operator==(const SyncFirstUseMark &, const SyncFirstUseMark &) = default;
};

// First use of a sync chat by an account with no state in its space (core PlanSyncFirstUse).
struct SyncFirstUseReview {
	SyncFirstUseNeed need = SyncFirstUseNeed::None;
	std::optional<SyncFirstUseAction> action;
	SyncFirstUseInput input;
	SyncConfigPublishRequest request;
	std::optional<QString> headKey;
	bool sameBytes = false;
	bool carry = false;
	bool settled = false;
};

[[nodiscard]] bool SyncCarryLeftoversPresent(const QString &syncDirectory);
[[nodiscard]] std::optional<SyncFirstUseMark> ReadSyncFirstUseMark(const AccountFiles &files);
[[nodiscard]] bool WriteSyncFirstUseMark(const AccountFiles &files, const SyncFirstUseMark &mark);
bool ClearSyncFirstUseMark(const AccountFiles &files);
[[nodiscard]] SyncFirstUseReview ReviewSyncFirstUse(
	const AccountFiles &files,
	const SyncAccountInventoryResult &inventory,
	SyncFirstUseChoice choice = SyncFirstUseChoice::None);
[[nodiscard]] SyncConfigReview ReviewSyncConfig(
	Main::Account &account,
	Main::Session &session,
	const SyncAccountInventoryResult &inventory,
	PeerToken chat = {});

} // namespace Purple

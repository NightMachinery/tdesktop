/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_sync_account_setup.h"
#include "purple/purple_sync_config_history.h"
#include "purple/purple_sync_config_review.h"
#include "purple/purple_sync_local_store.h"

#include <optional>
#include <vector>

namespace Main {
class Account;
class Session;
}

namespace Purple {

struct SyncConfigApplyResult {
	SyncConfigApplyStatus status = SyncConfigApplyStatus::NeedsReview;
	std::optional<SyncStoreStatus> storeStatus;
	std::optional<SyncAccountSetupStatus> setupStatus;
	QString historyId;
	QString fingerprint;
	ConfigSyncVerdict nextVerdict = ConfigSyncVerdict::Invalid;
	std::vector<QString> expectedParents;
	bool joined = false;
	bool wroteFile = false;
	bool undoAvailable = false;
	bool otherVersionsRemain = false;
	bool adopted = false;
	bool publishNeeded = false;
};

[[nodiscard]] SyncConfigApplyResult ApplySyncConfigChoice(
	Main::Account &account,
	Main::Session &session,
	const SyncAccountInventoryResult &inventory,
	const SyncConfigReview &review,
	const std::optional<QString> &chosenRemoteKey);

struct SyncConfigRestoreResult {
	SyncConfigRestoreStatus status = SyncConfigRestoreStatus::NotFound;
	QString historyId;
	QString fingerprint;
};

[[nodiscard]] SyncConfigRestoreResult RestoreSyncConfigHistory(
	const QString &id,
	SyncConfigHistoryReason reason);

} // namespace Purple

/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_sync_account_inventory.h"
#include "purple/purple_sync_local_store.h"

#include <optional>

namespace Main {
class Account;
class Session;
}

namespace Purple {

enum class SyncAccountSetupStatus {
	Ready,
	AlreadyBound,
	AccountUnavailable,
	IncompleteInventory,
	NeedsReview,
	AccountUnbound,
	StoreError,
	InvalidGeneratedState,
};

struct SyncAccountSetupResult {
	SyncAccountSetupStatus status = SyncAccountSetupStatus::IncompleteInventory;
	std::optional<SyncStoreStatus> storeStatus;
	QString space;
	bool createdSpace = false;
};

[[nodiscard]] SyncAccountSetupResult InitializeSyncAccountLocally(
	Main::Account &account,
	Main::Session &session,
	const SyncAccountInventoryResult &inventory);

} // namespace Purple

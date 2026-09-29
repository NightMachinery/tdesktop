/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/weak_ptr.h"
#include "purple/purple_sync_account_inventory.h"
#include "purple/purple_sync_config_post.h"
#include "purple/purple_sync_local_store.h"
#include "purple/purple_sync_own_inventory.h"

#include <memory>
#include <optional>

namespace Main {
class Account;
class Session;
}

namespace Purple {

enum class SyncConfigPublishStatus {
	Confirmed,
	AlreadySynced,
	NeedsReview,
	CloneDetected,
	Incomplete,
	AccountUnavailable,
	AccountUnbound,
	StoreError,
	InvalidSettings,
	OutcomeUnknown,
	Cancelled,
};

struct SyncConfigPublishResult {
	SyncConfigPublishStatus status = SyncConfigPublishStatus::Incomplete;
	std::optional<SyncStoreStatus> storeStatus;
	int32_t messageId = 0;
};

class SyncConfigPublish final : public base::has_weak_ptr {
public:
	SyncConfigPublish(
		Main::Account &account,
		Main::Session &session,
		SyncAccountInventoryResult inventory,
		Fn<void(SyncConfigPublishResult)> finished);
	~SyncConfigPublish();

	SyncConfigPublish(const SyncConfigPublish &) = delete;
	SyncConfigPublish &operator=(const SyncConfigPublish &) = delete;

	void Start();
	void Cancel();

private:
	[[nodiscard]] bool AccountAvailable() const;
	void PlanPost(
		const SyncLocalState &state,
		const SyncOwnInventoryResult &own,
		const QByteArray &staged,
		const QByteArray &token);
	void OnPost(SyncConfigPostResult result);
	void Finish(SyncConfigPublishResult result);

	base::weak_ptr<Main::Account> _account;
	base::weak_ptr<Main::Session> _session;
	SyncAccountInventoryResult _inventory;
	Fn<void(SyncConfigPublishResult)> _finished;
	std::unique_ptr<SyncLocalStore> _store;
	std::unique_ptr<SyncConfigPost> _post;
	bool _started = false;
	bool _done = false;
	bool _posted = false;
};

} // namespace Purple

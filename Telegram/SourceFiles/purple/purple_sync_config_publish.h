/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/weak_ptr.h"
#include "purple/purple_sync_chat_carry_gate.h"
#include "crl/crl_object_on_queue.h"
#include "purple/purple_sync_config_publish_flow.h"
#include "purple/purple_sync_account_inventory.h"
#include "purple/purple_sync_config_post.h"
#include "purple/purple_sync_config_review.h"
#include "purple/purple_sync_inventory.h"
#include "purple/purple_sync_local_store.h"

#include <memory>
#include <optional>
#include <vector>

namespace Main {
class Account;
class Session;
}

namespace Purple {

class SyncConfigPublish final : public base::has_weak_ptr {
public:
	SyncConfigPublish(
		Main::Account &account,
		Main::Session &session,
		SyncAccountInventoryResult inventory,
		SyncConfigSendQueue scanQueue,
		uint64_t scanPosts,
		Fn<void(SyncConfigPublishResult)> finished,
		SyncConfigPublishRequest request);
	SyncConfigPublish(
		Main::Account &account,
		Main::Session &session,
		DesktopSyncChatInventory inventory,
		SyncConfigSendQueue scanQueue,
		uint64_t scanPosts,
		Fn<void(SyncConfigPublishResult)> finished,
		SyncConfigPublishRequest request,
		std::optional<DesktopSyncChatSetupSnapshot> setup = std::nullopt,
		Fn<bool()> current = {},
		bool initializeOnly = false);
	~SyncConfigPublish();

	SyncConfigPublish(const SyncConfigPublish &) = delete;
	SyncConfigPublish &operator=(const SyncConfigPublish &) = delete;

	void Start();
	void Cancel();

private:
	[[nodiscard]] bool AccountAvailable() const;
	void Receive(SyncConfigPublishDiskReply reply);
	void Renew(Fn<void(bool)> ready);
	void StartPost(const QByteArray &staged);
	void OnPost(SyncConfigPostResult result);
	void Finish(SyncConfigPublishResult result);

	base::weak_ptr<Main::Account> _account;
	base::weak_ptr<Main::Session> _session;
	SyncAccountInventoryResult _inventory;
	std::optional<DesktopSyncChatIo> _inventoryRoute;
	SyncConfigSendQueue _scanQueue = SyncConfigSendQueue::Empty;
	uint64_t _scanPosts = 0;
	Fn<void(SyncConfigPublishResult)> _finished;
	SyncConfigPublishRequest _request;
	std::optional<AccountRoutingSnapshot> _routing;
	std::optional<DesktopSyncChatSetupSnapshot> _setup;
	Fn<bool()> _current;
	QByteArray _bindingToken;
	QString _directory;
	std::shared_ptr<std::atomic<bool>> _admitted;
	std::shared_ptr<DesktopSyncCarryGate::Lease> _carryWriter;
	std::unique_ptr<crl::object_on_queue<SyncConfigPublishFlow>> _worker;
	rpl::lifetime _lifetime;
	std::unique_ptr<SyncConfigPost> _post;
	bool _started = false;
	bool _done = false;
	bool _posted = false;
	bool _initializeOnly = false;
};

} // namespace Purple

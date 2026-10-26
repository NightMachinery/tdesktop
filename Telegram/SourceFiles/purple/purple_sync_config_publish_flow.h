#pragma once

#include "purple/purple_account_config.h"
#include "purple/purple_sync_chat_setup.h"
#include "purple/purple_sync_local_store.h"

#include <atomic>

namespace Purple {

struct SyncConfigPublishResult {
	SyncConfigPublishStatus status = SyncConfigPublishStatus::Incomplete;
	std::optional<SyncStoreStatus> storeStatus;
	int32_t messageId = 0;
};

enum class SyncConfigPublishDiskStep { Binding, Post, Finished };

struct SyncConfigPublishDiskInput {
	AccountRoutingSnapshot routing;
	AccountKey account;
	PeerToken chat;
	QByteArray binding;
	std::vector<SyncRecoveryPreference> otherPreferences;
	std::optional<DesktopSyncChatSetupSnapshot> setup;
	SyncAccountInventoryResult inventory;
	SyncConfigPublishRequest request;
	SyncConfigWriter writer;
	SyncConfigSendQueue queue = SyncConfigSendQueue::HoldsSyncRecord;
	int64_t now = 0;
	bool initializeOnly = false;
};

struct SyncConfigPublishDiskReply {
	SyncConfigPublishDiskStep step = SyncConfigPublishDiskStep::Finished;
	SyncConfigPublishResult result;
	std::optional<SyncLocalState> initial;
	QString directory;
	QByteArray staged;
};

class SyncConfigPublishFlow final {
public:
	SyncConfigPublishFlow(SyncConfigPublishDiskInput input,
		std::shared_ptr<std::atomic<bool>> admitted,
		Fn<void(SyncConfigPublishDiskReply)> reply);
	void start();
	void bindingCommitted(QByteArray token);
	[[nodiscard]] bool authorize();
	void confirm(QByteArray readback, int32_t messageId);
	void cancel();

private:
	bool current() const;
	bool setupAllowed();
	bool recoveryAllowed();
	void prepare();
	void post(QByteArray staged);
	void finish(SyncConfigPublishResult result);

	SyncConfigPublishDiskInput _input;
	std::shared_ptr<std::atomic<bool>> _admitted;
	Fn<void(SyncConfigPublishDiskReply)> _reply;
	DesktopAccountConfig _routing;
	std::optional<AccountSettingsOpenTarget> _opening;
	std::optional<AccountOperationTarget> _target;
	std::unique_ptr<SyncLocalStore> _store;
	SettingsFileInput _storeInput;
	QString _setupPath;
	QByteArray _setupIdentity;
	QByteArray _setupBytes;
	bool _bindingPending = false;
	bool _done = false;

};

}

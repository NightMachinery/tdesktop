#pragma once

#include "purple/purple_sync_chat_io.h"
#include "purple/purple_account_config.h"
#include <rpl/producer.h>

namespace Main { class Session; }

namespace Purple {

enum class DesktopSyncChatPhase { Unset, Searching, Use, Paused, Lost };

struct DesktopSyncChatStatus {
	DesktopSyncChatPhase phase = DesktopSyncChatPhase::Paused;
	std::optional<PeerToken> chat;
	QString reason;
	bool complete = false;
	bool postingHeld = true;
	int64_t retryAt = 0;
};

struct DesktopSyncChatOperation {
	QString directory;
	AccountKey account;
	Fn<bool()> current;
	std::optional<AccountRoutingSnapshot> routing;
	Fn<void(Fn<void(bool)>)> authorize;
};

struct DesktopSyncChatLease {
	DesktopSyncChatIo route;
	Fn<bool()> current;
};

[[nodiscard]] bool SyncChatIoBelongsToSession(
	not_null<Main::Session*> session, const DesktopSyncChatIo &route);
[[nodiscard]] std::optional<DesktopSyncChatLease> SyncChatIoForPost(
	not_null<Main::Session*> session, PeerToken chat);
void StartSyncChatService();
void EnsureSyncChatResolved(not_null<Main::Session*> session, bool force = false);
void CheckSyncChatBeforePost(
	not_null<Main::Session*> session,
	Fn<void(DesktopSyncChatStatus)> done,
	DesktopSyncChatOperation operation = {});
[[nodiscard]] DesktopSyncChatStatus SyncChatStatusOf(not_null<Main::Session*> session);
[[nodiscard]] rpl::producer<Main::Session*> SyncChatStatusChanges();

}

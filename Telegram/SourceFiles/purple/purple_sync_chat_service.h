#pragma once

#include "purple/purple_sync_chat_setup.h"
#include "purple/purple_account_config.h"
#include <rpl/producer.h>

namespace Main {
class Session;
}

namespace Purple {

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

[[nodiscard]] std::optional<DesktopSyncChatSetupSnapshot> SyncChatSetupSnapshotOf(
	not_null<Main::Session*> session);

[[nodiscard]] bool SyncChatSetupContextCurrent(
	not_null<Main::Session*> session, const DesktopSyncChatDiscoveryScope &scope);
[[nodiscard]] bool CachedSyncChatSetupContextCurrent(
	not_null<Main::Session*> session, const DesktopSyncChatDiscoveryScope &scope);

void StartSyncChatDiscovery();
void EnsureSyncChatResolved(
	not_null<Main::Session*> session, bool force = false);
void CheckSyncChatBeforePost(
	not_null<Main::Session*> session,
	Fn<void(DesktopSyncChatStatus)> done,
	DesktopSyncChatOperation operation = {});
[[nodiscard]] DesktopSyncChatStatus
SyncChatStatusOf(not_null<Main::Session*> session);
[[nodiscard]] bool SyncChatSelfResolved(not_null<Main::Session*> session);
[[nodiscard]] rpl::producer<Main::Session*> SyncChatStatusChanges();

}

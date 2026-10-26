#pragma once

#include "purple/purple_sync_chat_discovery.h"
#include <rpl/producer.h>

namespace Main {
class Session;
}

namespace Purple {

struct DesktopSyncChatOperation {
	QString directory;
	AccountKey account;
	Fn<bool()> current;
};

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

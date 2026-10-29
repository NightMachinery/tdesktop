#pragma once

#include "purple/purple_sync_chat_mark_actions.h"

namespace Purple {

class SyncRequestSender;

[[nodiscard]] bool SendDesktopSyncChatMarkDelete(SyncRequestSender &sender,
	DesktopSyncChatMarkPermit permit, DesktopSyncChatMarkRenewal renew, Fn<void(Fn<void()>)> onSender,
	Fn<void(std::optional<MTPmessages_AffectedMessages>)> done);

}

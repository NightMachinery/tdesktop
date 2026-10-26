#pragma once

#include "purple/purple_sync_chat_create.h"

namespace Purple {

using DesktopSyncChatSetupWriteRequest = std::variant<MTPchannels_CreateChannel,
	MTPmessages_SendMessage, MTPmessages_UpdatePinnedMessage,
	MTPaccount_UpdateNotifySettings, MTPfolders_EditPeerFolders>;

[[nodiscard]] std::optional<DesktopSyncChatSetupWriteRequest> DesktopSyncChatSetupRequest(
	const DesktopSyncChatCreateCommand &command,
	const std::optional<DesktopSyncChatIo> &route);

}

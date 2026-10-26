#pragma once

#include "purple/purple_sync_chat_io.h"

namespace Purple {

[[nodiscard]] bool CompatibleDesktopSyncChatIo(
	const DesktopSyncChatIo &opening, const DesktopSyncChatIo &fresh);
[[nodiscard]] std::optional<MTPmessages_SendMedia> DesktopSyncChatSendMediaRequest(
	const DesktopSyncChatIo &route, const MTPInputMedia &media, uint64_t randomId);
[[nodiscard]] std::optional<MTPmessages_EditMessage> DesktopSyncChatEditRequest(
	const DesktopSyncChatIo &route, const MTPInputMedia &media, int32_t id);
[[nodiscard]] std::optional<int32_t> DesktopSyncChatUploadReceipt(
	const DesktopSyncChatIo &route, const MTPUpdates &updates,
	uint64_t randomId, int32_t editId = 0);

}

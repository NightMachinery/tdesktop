#pragma once

#include "purple/purple_sync_chat_create.h"
#include "purple/purple_sync_chat_io.h"
#include "purple/purple_sync_retry.h"

namespace Purple {

using DesktopSyncChatDecoration = std::variant<
	MTPmessages_UpdatePinnedMessage,
	MTPaccount_UpdateNotifySettings,
	MTPfolders_EditPeerFolders>;

[[nodiscard]] std::optional<MTPmessages_SendMessage> DesktopSyncChatMarkRequest(
	const DesktopSyncChatIo &route, uint64_t randomId);
[[nodiscard]] std::optional<DesktopSyncChatDecoration> DesktopSyncChatDecorationRequest(
	const DesktopSyncChatCreateCommand &command, const DesktopSyncChatIo &route);
[[nodiscard]] QByteArray EncodeDesktopSyncChatSetupUpdates(const MTPUpdates &updates);
[[nodiscard]] std::optional<MTPUpdates> DecodeDesktopSyncChatSetupUpdates(const QByteArray &bytes);
[[nodiscard]] DesktopSyncChatCreateOutcome DesktopSyncChatSetupError(const QString &type);

}

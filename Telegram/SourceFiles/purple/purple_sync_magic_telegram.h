#pragma once

#include "purple/purple_sync_magic_chat.h"

class ChannelData;

namespace Main {
class Session;
} // namespace Main

namespace Purple {

inline constexpr auto kSyncMagicBindingFile = "chat.json";
inline constexpr auto kSyncMagicBindingLimit = 16 * 1024;

[[nodiscard]] std::optional<MagicChatCandidate> SyncMagicCandidate(
	const MTPChat &chat,
	const MTPChatFull *full);
[[nodiscard]] MagicChatCandidate SyncMagicLocalCandidate(
	not_null<ChannelData*> channel);

[[nodiscard]] std::shared_ptr<SyncMagicPort> MakeSyncMagicTelegramPort(
	not_null<Main::Session*> session,
	QString bindingPath,
	Fn<bool()> current);

} // namespace Purple

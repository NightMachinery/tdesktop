#pragma once

#include "purple/purple_sync_chat_service.h"
#include "purple/purple_sync_magic_chat.h"

namespace Purple {

[[nodiscard]] DesktopSyncChatStatus SyncMagicStatusOf(
	not_null<Main::Session*> session);
void SyncMagicLookup(
	not_null<Main::Session*> session,
	SyncMagicTrigger trigger,
	bool fresh,
	Fn<void(DesktopSyncChatStatus)> done = nullptr);
[[nodiscard]] std::optional<DesktopSyncChatLease> SyncMagicLease(
	not_null<Main::Session*> session,
	PeerToken chat);
[[nodiscard]] bool SyncMagicOwnsRoute(
	not_null<Main::Session*> session,
	const DesktopSyncChatIo &route);
[[nodiscard]] SyncMagicChat *SyncMagicChatOf(not_null<Main::Session*> session);
// The chat the last lookup bound; nullopt while it waits, has a problem or has not run.
[[nodiscard]] std::optional<PeerToken> SyncMagicBound(
	not_null<Main::Session*> session);
[[nodiscard]] PeerData *SyncMagicBoundPeer(not_null<Main::Session*> session);
[[nodiscard]] bool SyncMagicPaused(not_null<Main::Session*> session);
[[nodiscard]] rpl::producer<Main::Session*> SyncMagicChanges();

} // namespace Purple

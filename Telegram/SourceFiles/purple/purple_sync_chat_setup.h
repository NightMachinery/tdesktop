#pragma once

#include "purple/purple_sync_chat_discovery.h"

namespace Purple {

struct DesktopSyncChatSetupSnapshot {
	DesktopSyncChatDiscoveryScope scope;
	DesktopSyncChatStatus status;
	SyncChatResolveInput input;
	std::optional<DesktopSyncChatCacheRecord> cache;
	uint64_t run = 0;
};

enum class DesktopSyncChatSetupChoice { Create, Self, Existing };
enum class DesktopSyncChatSetupRefusal {
	None, Stale, Bound, Incomplete, Ineligible, UnknownReaders, ConsentRequired
};

struct DesktopSyncChatSetupSelection {
	DesktopSyncChatSetupChoice choice = DesktopSyncChatSetupChoice::Create;
	std::optional<DesktopSyncChatPeer> peer;
	bool readerConsent = false;
};

[[nodiscard]] bool SameDesktopSyncChatDisclosure(
	const DesktopSyncChatPeer &opening, const DesktopSyncChatPeer &fresh);
[[nodiscard]] bool DesktopSyncChatFreshSetup(
	const DesktopSyncChatSetupSnapshot &snapshot);
[[nodiscard]] DesktopSyncChatSetupRefusal DesktopSyncChatSetupEligibility(
	const DesktopSyncChatSetupSnapshot &snapshot,
	const DesktopSyncChatPeer &peer);
[[nodiscard]] DesktopSyncChatSetupRefusal AcceptDesktopSyncChatSetup(
	const DesktopSyncChatSetupSnapshot &opening,
	const DesktopSyncChatSetupSnapshot &fresh,
	const DesktopSyncChatSetupSelection &selection);

}

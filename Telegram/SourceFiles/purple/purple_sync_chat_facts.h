#pragma once

#include "purple/purple_local_choice.h"
#include "purple/purple_sync_chat.h"
#include "scheme.h"

namespace Purple {

struct DesktopSyncChatOrigin {
	AccountKey account;
	QString login;

	[[nodiscard]] bool operator==(const DesktopSyncChatOrigin &) const = default;
};

struct DesktopSyncChatPeer {
	DesktopSyncChatOrigin origin;
	PeerToken chat;
	SyncChatKind kind = SyncChatKind::Unknown;
	std::optional<bool> creator;
	std::optional<bool> soleAdmin;
	std::optional<SyncChatFacts> facts;
	std::optional<std::vector<QString>> readerMembers;
};

struct DesktopSyncChatPeerInput {
	DesktopSyncChatOrigin origin;
	PeerToken chat;
	std::optional<MTPUser> user;
	std::optional<MTPChat> group;
	std::optional<MTPUserFull> userFull;
	std::optional<MTPChatFull> groupFull;
	std::optional<bool> canSendFiles;
	std::optional<PeerToken> chosenSendAs;
};

[[nodiscard]] bool ValidDesktopSyncChatOrigin(const DesktopSyncChatOrigin &origin);
[[nodiscard]] std::optional<PeerToken> DesktopSyncChatToken(
	const MTPPeer &peer, uint64_t selfUserId);
[[nodiscard]] DesktopSyncChatPeer MapDesktopSyncChatPeer(
	const DesktopSyncChatOrigin &origin,
	const DesktopSyncChatPeerInput &input);
[[nodiscard]] SyncChatMessageFacts MapDesktopSyncChatMessage(
	const DesktopSyncChatOrigin &origin,
	const DesktopSyncChatPeer &peer,
	const MTPMessage &message);
[[nodiscard]] std::optional<SyncChatEligibilityResult> DesktopSyncChatEligibility(
	const DesktopSyncChatPeer &peer);

}

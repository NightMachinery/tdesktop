#pragma once

#include "data/data_msg_id.h"
#include "purple/purple_sync_chat_requests.h"
#include "purple/purple_sync_chat_scope.h"
#include "purple/purple_sync_inventory.h"

namespace Purple {

struct DesktopSyncChatIo {
	DesktopSyncChatPeer peer;
	MTPInputPeer input;
};

struct DesktopSyncChatRecordReceipt {
	DesktopSyncChatIo route;
	MTPmessages_Messages response;
	SyncCandidateRecord record;
};

struct DesktopSyncChatInventory {
	DesktopSyncChatIo route;
	SyncAccountInventoryResult inventory;
	std::vector<DesktopSyncChatRecordReceipt> receipts;
};

[[nodiscard]] bool ValidDesktopSyncChatReadIo(const DesktopSyncChatIo &route);
[[nodiscard]] bool ValidDesktopSyncChatIo(const DesktopSyncChatIo &route);
[[nodiscard]] std::optional<MTPInputChannel> DesktopSyncChatInputChannel(
	const DesktopSyncChatIo &route);
[[nodiscard]] std::optional<MTPmessages_GetHistory> DesktopSyncChatHistoryRequest(
	const DesktopSyncChatIo &route, int offset);
[[nodiscard]] auto DesktopSyncChatRecordRequest(
	const DesktopSyncChatIo &route, int id)
-> std::optional<DesktopSyncChatExactRequest>;
[[nodiscard]] std::optional<MTPMessage> DesktopSyncChatRecordMessage(
	const DesktopSyncChatIo &route,
	const MTPmessages_Messages &response,
	int id);
[[nodiscard]] std::optional<DesktopSyncChatRecordReceipt> SealDesktopSyncChatRecord(
	const DesktopSyncChatIo &route,
	const MTPmessages_Messages &response,
	const SyncCandidateRecord &record,
	uint64_t documentId,
	uint64_t editDate);
[[nodiscard]] std::optional<FullMsgId> DesktopSyncChatFileOrigin(
	const DesktopSyncChatIo &route, int id);
[[nodiscard]] bool DesktopSyncChatInventoryMatches(
	const DesktopSyncChatInventory &inventory,
	const DesktopSyncChatOrigin &origin,
	PeerToken chat);

}

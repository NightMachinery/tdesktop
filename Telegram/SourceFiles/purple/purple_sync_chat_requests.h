#pragma once

#include "purple/purple_sync_chat_facts.h"

#include <variant>

namespace Purple {







enum class DesktopSyncChatPageStatus { Next, Complete, Capped, Incomplete };

using DesktopSyncChatExactRequest =
	std::variant<MTPmessages_GetMessages, MTPchannels_GetMessages>;
[[nodiscard]] auto DesktopSyncChatExactReadRequest(
	PeerToken chat, int id, const std::optional<MTPInputChannel> &channel)
-> std::optional<DesktopSyncChatExactRequest>;
[[nodiscard]] bool RecognizedDesktopSyncChatRead(
	const MTPmessages_Messages &response, PeerToken chat);
inline constexpr auto kDesktopSyncChatInventoryPages = 1000;

class DesktopSyncChatHistoryGuard final {
public:
	explicit DesktopSyncChatHistoryGuard(
		uint64_t selfUserId, PeerToken chat = {});
	[[nodiscard]] DesktopSyncChatPageStatus Add(
		const MTPmessages_Messages &response, uint64_t seen);

private:
	PeerToken _chat;
	uint64_t _selfUserId = 0;
	uint64_t _seen = 0;
	int _pages = 0;
	std::optional<uint64_t> _total;
	bool _finished = false;

};



}

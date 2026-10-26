#pragma once

#include "purple/purple_sync_chat_facts.h"

#include <set>
#include <tuple>
#include <variant>

namespace Purple {

enum class DesktopSyncChatSearch {
	Global,
	Broadcasts,
	Self,
	Dialog,
	SettingsHint
};

struct DesktopSyncChatCursor {
	int rate = 0;
	std::optional<PeerToken> peer;
	int id = 0;

	[[nodiscard]] bool
	operator==(const DesktopSyncChatCursor &) const = default;
};

struct DesktopSyncChatSearchPage {
	std::vector<SyncChatCache> positions;
	std::optional<int> nextRate;
	int total = 0;
	bool full = false;
	bool recognized = false;
};

enum class DesktopSyncChatPageStatus { Next, Complete, Capped, Incomplete };

[[nodiscard]] MTPmessages_SearchGlobal DesktopSyncChatGlobalRequest(
	bool broadcastsOnly,
	const DesktopSyncChatCursor &cursor,
	const MTPInputPeer &offset);
[[nodiscard]] MTPmessages_Search DesktopSyncChatSearchRequest(
	DesktopSyncChatSearch kind, const MTPInputPeer &peer, int offsetId = 0);
using DesktopSyncChatExactRequest =
	std::variant<MTPmessages_GetMessages, MTPchannels_GetMessages>;
[[nodiscard]] auto DesktopSyncChatExactReadRequest(
	PeerToken chat, int id, const std::optional<MTPInputChannel> &channel)
-> std::optional<DesktopSyncChatExactRequest>;
[[nodiscard]] bool RecognizedDesktopSyncChatRead(
	const MTPmessages_Messages &response, PeerToken chat);
[[nodiscard]] DesktopSyncChatSearchPage MapDesktopSyncChatSearchPage(
	const DesktopSyncChatOrigin &origin,
	const MTPmessages_Messages &response,
	DesktopSyncChatSearch kind,
	std::optional<PeerToken> target);

class DesktopSyncChatHistoryGuard final {
public:
	explicit DesktopSyncChatHistoryGuard(uint64_t selfUserId);
	[[nodiscard]] DesktopSyncChatPageStatus Add(
		const MTPmessages_Messages &response, uint64_t seen);

private:
	uint64_t _selfUserId = 0;
	uint64_t _seen = 0;
	int _pages = 0;
	std::optional<uint64_t> _total;
	bool _finished = false;

};

class DesktopSyncChatSearchPages final {
public:
	DesktopSyncChatSearchPages(
		DesktopSyncChatOrigin origin, DesktopSyncChatSearch kind);
	[[nodiscard]] DesktopSyncChatPageStatus
	Add(const DesktopSyncChatSearchPage &page);
	[[nodiscard]] DesktopSyncChatCursor cursor() const;
	[[nodiscard]] int pages() const;

private:
	DesktopSyncChatOrigin _origin;
	DesktopSyncChatSearch _kind;
	DesktopSyncChatCursor _cursor;
	int _pages = 0;
	std::optional<int> _total;
	std::set<std::pair<QString, int>> _seen;
	std::set<std::tuple<int, QString, int>> _cursors;
	bool _finished = false;
};

}

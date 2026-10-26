#pragma once

#include "purple/purple_sync_chat_cache.h"
#include "purple/purple_sync_chat_io.h"
#include "purple/purple_sync_chat_requests.h"
#include "purple/purple_sync_inventory.h"

#include <map>
#include <memory>

namespace Purple {

enum class DesktopSyncChatPhase { Unset, Searching, Use, Paused, Lost };
enum class DesktopSyncChatStep {
	Search,
	PeerInfo,
	ExactMark,
	SettingsHint,
	Inventory,
	Idle
};

struct DesktopSyncChatStatus {
	DesktopSyncChatPhase phase = DesktopSyncChatPhase::Paused;
	SyncChatResolution resolution;
	QString reason;
	bool complete = false;
	bool postingHeld = true;
	int64_t retryAt = 0;
};

struct DesktopSyncChatCommand {
	DesktopSyncChatOrigin origin;
	uint64_t ticket = 0;
	DesktopSyncChatStep step = DesktopSyncChatStep::Idle;
	DesktopSyncChatSearch search = DesktopSyncChatSearch::Global;
	DesktopSyncChatCursor cursor;
	PeerToken chat;
	std::optional<SyncChatCache> mark;
	uint64_t run = 0;
};

struct DesktopSyncChatReply {
	DesktopSyncChatOrigin origin;
	uint64_t ticket = 0;
	bool success = false;
	std::optional<DesktopSyncChatSearchPage> page;
	std::vector<MTPMessage> messages;
	std::optional<DesktopSyncChatPeer> peer;
	std::optional<bool> chatReadable;
	bool confirmedChatGone = false;
	SyncChatSettingsHint hint = SyncChatSettingsHint::Unknown;
	SyncChatRecordPresence records = SyncChatRecordPresence::Unknown;
};

[[nodiscard]] DesktopSyncChatReply MapDesktopSyncChatInventoryReply(
	const DesktopSyncChatOrigin &origin,
	uint64_t ticket,
	const SyncAccountInventoryResult &inventory);

[[nodiscard]] DesktopSyncChatReply MapDesktopSyncChatInventoryReply(
	const DesktopSyncChatOrigin &origin,
	uint64_t ticket,
	PeerToken chat,
	const DesktopSyncChatInventory &inventory);

class DesktopSyncChatDiscovery final {
public:
	DesktopSyncChatDiscovery(
		DesktopSyncChatOrigin origin,
		std::optional<DesktopSyncChatCacheRecord> cache,
		bool settingsBound,
		PeerToken settingsChat,
		std::vector<PeerToken> loadedDialogs,
		QString loginBinding);
	void Start(int64_t now);
	void Tick(int64_t now);
	void Cancel();
	[[nodiscard]] bool Complete(const DesktopSyncChatReply &reply, int64_t now);
	[[nodiscard]] DesktopSyncChatCommand command() const;
	[[nodiscard]] const DesktopSyncChatStatus &status() const;
	[[nodiscard]] const SyncChatResolveInput &input() const;
	[[nodiscard]] std::optional<DesktopSyncChatPeer> peer(PeerToken chat) const;
	[[nodiscard]] auto cache() const
	-> const std::optional<DesktopSyncChatCacheRecord> &;

private:
	void Begin(int64_t now, bool retry);
	void Search(DesktopSyncChatSearch kind);
	void SearchDone(int64_t now);
	void PeerNext(int64_t now);
	void Resolve(int64_t now);
	void Finish(QString reason = {});
	void Issue(
		DesktopSyncChatStep step,
		PeerToken chat = {},
		std::optional<SyncChatCache> mark = {});
	void Remember(const std::vector<MTPMessage> &messages);
	void Exact(const DesktopSyncChatReply &reply, int64_t now);

	DesktopSyncChatOrigin _origin;
	QString _loginBinding;
	std::optional<DesktopSyncChatCacheRecord> _cache;
	SyncChatResolveInput _input;
	DesktopSyncChatStatus _status;
	DesktopSyncChatCommand _command;
	std::vector<PeerToken> _dialogs;
	std::unique_ptr<DesktopSyncChatSearchPages> _pages;
	std::vector<MTPMessage> _hits;
	std::vector<PeerToken> _peers;
	std::map<QString, DesktopSyncChatPeer> _facts;
	std::set<QString> _inventories;
	std::set<std::pair<QString, int>> _verified;
	size_t _peerIndex = 0;
	size_t _dialogIndex = 0;
	uint64_t _ticket = 0;
	bool _complete = true;
	bool _globalCapped = false;
	bool _fallbackUsed = false;
	bool _uncertain = false;
	bool _cacheRead = false;
	int _retries = 0;
};

}

#pragma once

#include "purple/purple_sync_chat_carry_io.h"

namespace Purple {

struct DesktopSyncCleanupItem {
	SyncCarryReference reference;
	int64_t date = 0;
	bool mark = false;
	QString key;
	[[nodiscard]] bool operator==(const DesktopSyncCleanupItem &) const = default;
};

struct DesktopSyncCleanupRead {
	DesktopSyncCleanupItem item;
	uint64_t generation = 0;
	DesktopSyncChatReadStamp after;
	std::shared_ptr<const int> owner;
};

class DesktopSyncCleanupPermit final {
public:
	[[nodiscard]] bool operationCurrent() const;
	[[nodiscard]] bool current() const;
	[[nodiscard]] std::optional<DesktopSyncChatDelete> request() const;
	[[nodiscard]] bool successor(const DesktopSyncCleanupPermit &next) const;
	[[nodiscard]] const DesktopSyncCleanupItem &item() const;
	[[nodiscard]] const DesktopSyncChatIo &route() const;
	void revoke() const;

private:
	friend class DesktopSyncChatCleanup;
	DesktopSyncCleanupItem _item;
	DesktopSyncChatIo _route;
	QString _operation;
	uint64_t _revision = 0;
	uint64_t _generation = 0;
	std::shared_ptr<std::atomic<bool>> _operationLive;
	std::shared_ptr<std::atomic<bool>> _admitted;
	std::shared_ptr<std::atomic<uint64_t>> _epoch;
	uint64_t _openingEpoch = 0;
	std::shared_ptr<std::atomic<uint64_t>> _slot;
	std::shared_ptr<std::atomic<bool>> _claimed;
};

struct DesktopSyncCleanupProgress {
	bool pending = false;
	bool canAct = false;
	bool skipped = false;
	bool uncopied = false;
	size_t confirmed = 0;
};

[[nodiscard]] bool DesktopSyncChatCleanupActivated();
[[nodiscard]] DesktopSyncCleanupProgress DesktopSyncChatCleanupProgressOf(
	const SyncCarryJournal &journal, bool pendingRequest, size_t confirmed = 0, bool uncopied = false);

class DesktopSyncChatCleanup final {
public:
	DesktopSyncChatCleanup(SyncChatCarryStore &store, DesktopSyncChatOrigin origin,
		std::shared_ptr<std::atomic<bool>> admitted);
	~DesktopSyncChatCleanup();
	[[nodiscard]] bool prepare(const DesktopSyncCarryFresh &fresh,
		const DesktopSyncChatInventory &destination, const SyncPlaylistCarryReceipt &playlist);
	[[nodiscard]] std::optional<DesktopSyncCleanupRead> next();
	[[nodiscard]] bool inspect(const DesktopSyncCleanupRead &read, const DesktopSyncCarryFresh &fresh,
		const DesktopSyncChatInventory &destination, const SyncPlaylistCarryReceipt &playlist,
		const MTPmessages_Messages &response, const std::optional<DesktopSyncChatRecordReceipt> &record);
	[[nodiscard]] std::optional<DesktopSyncCleanupPermit> permit() const;
	[[nodiscard]] std::optional<std::vector<SyncCarryReference>> deleted() const;
	[[nodiscard]] SyncCarryMarkReadback oldMarkAbsent() const;
	[[nodiscard]] bool uncopied() const;
	void revoke();

private:
	[[nodiscard]] bool context(const DesktopSyncCarryFresh &fresh,
		const DesktopSyncChatInventory &destination, const SyncPlaylistCarryReceipt &playlist);
	[[nodiscard]] bool save(const DesktopSyncCleanupItem &item, const QString &state, const QByteArray &body);
	SyncChatCarryStore &_store;
	DesktopSyncChatOrigin _origin;
	std::shared_ptr<std::atomic<bool>> _admitted;
	std::shared_ptr<std::atomic<bool>> _live = std::make_shared<std::atomic<bool>>(true);
	std::shared_ptr<std::atomic<uint64_t>> _slot = std::make_shared<std::atomic<uint64_t>>(0);
	std::shared_ptr<const int> _identity = std::make_shared<const int>(0);
	std::optional<DesktopSyncCleanupRead> _read;
	std::optional<DesktopSyncCleanupPermit> _permit;
	std::vector<DesktopSyncCleanupItem> _items;
	bool _uncopied = false;
};

class SyncRequestSender;
using DesktopSyncCleanupRenew = Fn<void(Fn<void(std::optional<DesktopSyncCleanupPermit>)>)>;
[[nodiscard]] bool SendDesktopSyncChatCleanup(SyncRequestSender &sender,
	DesktopSyncCleanupPermit permit, DesktopSyncCleanupRenew renew, Fn<void(Fn<void()>)> onSender,
	Fn<void(int, Fn<void()>)> delay, Fn<void(std::optional<MTPmessages_AffectedMessages>)> done);

}

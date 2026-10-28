#pragma once

#include "purple/purple_sync_chat_carry_store.h"
#include "purple/purple_sync_chat_write.h"

#include <map>

namespace Purple {

enum class DesktopSyncCarryEffect { Forward, OwnRecord, Mark };
enum class DesktopSyncCarryReconcile { Held, Absent, Present };

struct DesktopSyncCarryIdentity {
	QString slot;
	uint64_t randomId = 0;
	uint64_t fileId = 0;
};

struct DesktopSyncCarryFresh {
	DesktopSyncChatIo source;
	DesktopSyncChatIo target;
	SyncChatResolveInput resolutionInput;
	QString loginAuthority;
	std::optional<DesktopSyncChatRecordReceipt> sourceRecord;
	bool readerConsentAccepted = false;
	std::optional<DesktopSyncChatPeer> consentPeer;
	Fn<bool()> current;
};

class DesktopSyncCarryPermit final {
public:
	[[nodiscard]] bool current() const;
	void revoke() const;
	[[nodiscard]] const DesktopSyncChatIo &source() const;
	[[nodiscard]] const DesktopSyncChatIo &target() const;
	[[nodiscard]] uint64_t randomId() const;
	[[nodiscard]] uint64_t fileId() const;
	[[nodiscard]] uint64_t renewal() const;
	[[nodiscard]] bool uploadMatches(const QByteArray &bytes) const;
	[[nodiscard]] QByteArray uploadContent() const;
	[[nodiscard]] bool compatible(const DesktopSyncCarryPermit &fresh) const;
	[[nodiscard]] bool readerWarning() const;

private:
	friend class DesktopSyncChatCarryIo;
	friend std::optional<MTPmessages_ForwardMessages> DesktopSyncCarryForwardRequest(const DesktopSyncCarryPermit &permit);
	friend std::optional<MTPmessages_SendMessage> DesktopSyncCarryMarkRequest(const DesktopSyncCarryPermit &permit);
	DesktopSyncCarryPermit() = default;
	QString _operation;
	uint64_t _revision = 0;
	uint64_t _renewal = 0;
	DesktopSyncCarryEffect _effect = DesktopSyncCarryEffect::Forward;
	DesktopSyncCarryIdentity _identity;
	QString _hash;
	QByteArray _content;
	int32_t _sourceId = 0;
	QString _mark;
	DesktopSyncChatIo _source;
	DesktopSyncChatIo _target;
	Fn<bool()> _current;
	std::shared_ptr<std::atomic<uint64_t>> _epoch;
	uint64_t _openingEpoch = 0;
	std::shared_ptr<std::atomic<bool>> _active;
	bool _readerWarning = false;

};

[[nodiscard]] std::optional<SyncCarryInventory> DesktopSyncChatCarryInventory(
	const DesktopSyncChatInventory &inventory);
[[nodiscard]] std::optional<SyncCarryMarkReadback> DesktopSyncChatCarryMarkReadback(
	const DesktopSyncChatIo &route, int32_t id,
	const MTPmessages_Messages &response, const QString &text);
[[nodiscard]] std::optional<int32_t> DesktopSyncCarryUpdatesReceipt(
	const DesktopSyncCarryPermit &permit, const MTPUpdates &updates,
	Fn<void(const MTPUpdates &, uint64_t)> applyUpdates);
[[nodiscard]] std::optional<MTPmessages_ForwardMessages> DesktopSyncCarryForwardRequest(
	const DesktopSyncCarryPermit &permit);
[[nodiscard]] std::optional<MTPmessages_SendMessage> DesktopSyncCarryMarkRequest(
	const DesktopSyncCarryPermit &permit);

class DesktopSyncChatCarryIo final {
public:
	explicit DesktopSyncChatCarryIo(SyncChatCarryStore &store);
	~DesktopSyncChatCarryIo();
	DesktopSyncChatCarryIo(const DesktopSyncChatCarryIo &) = delete;
	DesktopSyncChatCarryIo &operator=(const DesktopSyncChatCarryIo &) = delete;
	[[nodiscard]] std::optional<DesktopSyncCarryIdentity> Reserve(
		DesktopSyncCarryEffect effect, const QString &recordHash,
		const DesktopSyncCarryFresh &fresh, uint64_t proposedRandomId,
		uint64_t proposedFileId = 0);
	[[nodiscard]] std::optional<DesktopSyncCarryPermit> Authorize(
		const QString &slot, const DesktopSyncCarryFresh &fresh);
	[[nodiscard]] std::optional<DesktopSyncCarryPermit> Renew(
		const DesktopSyncCarryPermit &permit, const DesktopSyncCarryFresh &fresh);
	[[nodiscard]] std::optional<int32_t> RecordReceipt(
		const DesktopSyncCarryPermit &permit, const MTPUpdates &updates);
	[[nodiscard]] bool OutcomeUnknown(const DesktopSyncCarryPermit &permit);
	[[nodiscard]] DesktopSyncCarryReconcile Reconcile(
		const QString &slot, const DesktopSyncChatInventory &inventory,
		const DesktopSyncCarryFresh &fresh);
	[[nodiscard]] DesktopSyncCarryReconcile ReconcileMark(
		const QString &slot, const DesktopSyncChatIo &route, int32_t id,
		const MTPmessages_Messages &response, const DesktopSyncCarryFresh &fresh);
	[[nodiscard]] bool cleanupEnabled() const;

private:
	[[nodiscard]] bool Context(const DesktopSyncCarryFresh &fresh) const;
	[[nodiscard]] bool Fresh(const DesktopSyncCarryFresh &fresh) const;
	[[nodiscard]] std::optional<QJsonObject> Transport() const;
	[[nodiscard]] bool Save(QJsonObject transport);
	[[nodiscard]] bool Readers(QJsonObject &transport, const DesktopSyncCarryFresh &fresh);
	[[nodiscard]] std::optional<DesktopSyncCarryPermit> Permit(
		const QString &slot, const QJsonObject &entry, const QJsonObject &transport,
		const DesktopSyncCarryFresh &fresh, std::shared_ptr<std::atomic<bool>> active);
	SyncChatCarryStore &_store;
	std::map<QString, std::pair<uint64_t, std::weak_ptr<std::atomic<bool>>>> _active;

};

}

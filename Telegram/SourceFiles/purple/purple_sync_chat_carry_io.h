#pragma once

#include "purple/purple_sync_chat_carry_store.h"
#include "purple/purple_sync_chat_write.h"

#include <map>

namespace Purple {

enum class DesktopSyncCarryEffect { Forward, OwnRecord, Mark, Pending };
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
	bool fullResolutionComplete = false;
};

struct DesktopSyncCarryMarkState {
	QString state;
	int32_t receiptId = 0;
	bool inFlight = false;
};

struct DesktopSyncCarryMarkRecovery {
	int checks = 0;
	int64_t checkedAt = 0;
	int64_t nextAt = 0;
};

class DesktopSyncCarryPermit final {
public:
	[[nodiscard]] bool current() const;
	[[nodiscard]] bool claimUpload() const;
	void revoke() const;
	[[nodiscard]] const DesktopSyncChatIo &source() const;
	[[nodiscard]] const DesktopSyncChatIo &target() const;
	[[nodiscard]] uint64_t randomId() const;
	[[nodiscard]] int32_t sourceMessageId() const;
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
	std::shared_ptr<std::atomic<bool>> _uploadClaim;
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
	[[nodiscard]] bool ReconcileAll(const DesktopSyncChatInventory &inventory, const DesktopSyncCarryFresh &fresh);
	[[nodiscard]] int32_t MarkReceiptId() const;
	[[nodiscard]] std::optional<DesktopSyncCarryMarkState> MarkState() const;
	[[nodiscard]] std::optional<DesktopSyncCarryMarkRecovery> MarkRecovery() const;
	[[nodiscard]] std::optional<DesktopSyncCarryMarkRecovery> CheckedMarkRecovery(int64_t now);
	[[nodiscard]] bool RetainedMarkReceipt(const DesktopSyncCarryFresh &fresh, const MTPUpdates &updates);
	[[nodiscard]] bool cleanupEnabled() const;
	[[nodiscard]] bool AcceptReaders(const DesktopSyncChatPeer &peer, const QString &stream);
	[[nodiscard]] bool CheckReaders(const DesktopSyncCarryFresh &fresh);
	[[nodiscard]] std::optional<bool> PendingIssued() const;
	[[nodiscard]] bool RememberPendingIssued();
	[[nodiscard]] bool ConsumePendingRead(const DesktopSyncChatInventory &inventory);

private:
	friend class DesktopSyncChatCarry;
	[[nodiscard]] std::optional<DesktopSyncCarryPermit> PendingPermit(
		const DesktopSyncCarryFresh &fresh, const DesktopSyncChatInventory &inventory,
		uint64_t proposedRandomId, uint64_t proposedFileId);
	[[nodiscard]] bool ReadFresh(const QString &slot, const DesktopSyncChatInventory &inventory) const;
	[[nodiscard]] bool SaveGeneration(const QString &slot, QJsonObject entry, QJsonObject transport);
	[[nodiscard]] bool Context(const DesktopSyncCarryFresh &fresh) const;
	[[nodiscard]] bool Fresh(const DesktopSyncCarryFresh &fresh) const;
	[[nodiscard]] bool PendingFresh(const DesktopSyncCarryFresh &fresh) const;
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

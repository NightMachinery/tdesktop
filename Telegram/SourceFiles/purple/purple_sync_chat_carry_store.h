#pragma once

#include "purple/purple_sync_chat_journal.h"
#include "purple/purple_sync_local_store.h"
#include "purple/purple_sync_chat_facts.h"
#include "purple/purple_sync_chat_scope.h"

#include <QtCore/QJsonObject>
#include <QtCore/QThread>

#include <atomic>
#include <map>

namespace Purple {

inline constexpr auto kSyncCarryJournalStorageLimit = 4 * 1024 * 1024;
inline constexpr auto kSyncCarryObjectStorageLimit = 64 * 1024 * 1024;

struct SyncChatCarryStoreScope {
	AccountKey account;
	QByteArray binding;
	QString loginAuthority;
	QString cacheFile;
};

struct SyncChatCarryStorageResult {
	SyncStoreStatus status = SyncStoreStatus::InvalidTransition;
	SyncCarryJournalStatus journalStatus = SyncCarryJournalStatus::Invalid;
	QString objectHash;
	[[nodiscard]] explicit operator bool() const { return status == SyncStoreStatus::Ready; }
};

[[nodiscard]] bool DesktopSyncCarryReaderAuthority(
	const QJsonObject &snapshot, const DesktopSyncChatPeer &peer);

[[nodiscard]] QString DesktopSyncCarryLogin(const QString &loginBinding);

[[nodiscard]] std::optional<uint64_t> DesktopSyncCarryTransportId(const QJsonValue &value);
[[nodiscard]] std::optional<uint64_t> DesktopSyncCarryTransportGeneration(const QJsonObject &entry);
[[nodiscard]] bool DesktopSyncCarryTransportEntry(const QJsonObject &entry);
[[nodiscard]] std::optional<QString> DesktopSyncCarryTransportHash(
	const QJsonObject &transport, const QJsonObject &envelope, const SyncCarryJournal &journal);
[[nodiscard]] std::optional<QJsonObject> DesktopSyncCarryTransportOf(
	const QJsonObject &envelope, const SyncCarryJournal &journal);
[[nodiscard]] std::optional<bool> DesktopSyncCarryPendingIssued(
	const QJsonObject &envelope, const SyncCarryJournal &journal);

class SyncChatCarryStore final {
public:
	SyncChatCarryStore(QString directory, SyncChatCarryStoreScope scope,
		SyncDurableFault fault = {});
	~SyncChatCarryStore();
	SyncChatCarryStore(const SyncChatCarryStore &) = delete;
	SyncChatCarryStore &operator=(const SyncChatCarryStore &) = delete;
	[[nodiscard]] SyncChatCarryStorageResult Open();
	[[nodiscard]] SyncChatCarryStorageResult RemoveAccount(bool explicitlyConfirmed);
	void Close();
	[[nodiscard]] SyncChatCarryStorageResult Begin(const SyncCarryJournalStart &input, bool consentRequired = false);
	[[nodiscard]] SyncChatCarryStorageResult Advance(SyncCarryJournalEvent event);
	[[nodiscard]] SyncChatCarryStorageResult PrepareState(const SyncLocalState &state);
	[[nodiscard]] SyncChatCarryStorageResult StageOwn();
	[[nodiscard]] SyncChatCarryStorageResult CommitPreparedState();
	[[nodiscard]] SyncChatCarryStorageResult RetireStage();
	[[nodiscard]] SyncChatCarryStorageResult Resume();
	[[nodiscard]] SyncChatCarryStorageResult DiscardInterrupted(bool confirmed);
	[[nodiscard]] SyncChatCarryStorageResult WriteCache(const DesktopSyncChatPeer &peer, bool complete);
	[[nodiscard]] SyncStoreResult Pending() const;
	[[nodiscard]] bool foreignLogin() const;
	[[nodiscard]] std::optional<std::vector<SyncCarryJournal>> RetainedEffects() const;
	[[nodiscard]] std::optional<std::vector<QByteArray>> RetainedImages() const;
	[[nodiscard]] const SyncCarryJournal *journal() const;
	[[nodiscard]] const SyncLocalState *state() const;
	[[nodiscard]] SyncStoreResult OwnBase() const;

private:
	friend class DesktopSyncChatCarryIo;
	friend class DesktopSyncChatCarry;
	[[nodiscard]] bool Current() const;
	[[nodiscard]] bool StateMatches() const;
	[[nodiscard]] SyncStoreStatus LoadObjects();
	[[nodiscard]] SyncChatCarryStorageResult Fail(SyncStoreStatus status);
	[[nodiscard]] SyncChatCarryStorageResult Persist(const SyncCarryJournalResult &next);
	[[nodiscard]] SyncChatCarryStorageResult StoreObject(const QString &kind, const QByteArray &bytes);
	[[nodiscard]] std::optional<QByteArray> ReadObject(const QString &kind, const QString &hash) const;
	[[nodiscard]] SyncStoreResult FindBase(const SyncLocalState &state) const;
	[[nodiscard]] SyncStoreStatus WitnessState(const QString &hash);
	[[nodiscard]] SyncStoreStatus WitnessCache(const SyncCarryCacheReceipt &receipt);
	[[nodiscard]] SyncChatCarryStorageResult Apply(const SyncCarryJournalEvent &event);
	[[nodiscard]] SyncStoreStatus InstallTargetPending(bool interrupted = false);
	[[nodiscard]] SyncStoreStatus RestoreInterruptedPending();
	[[nodiscard]] bool RememberPendingIssued();
	[[nodiscard]] SyncChatCarryStorageResult ArchiveTerminal(bool clear = true);
	[[nodiscard]] QString ObjectPath(const QString &kind, const QString &hash) const;

	QString _directory;
	SyncChatCarryStoreScope _scope;
	SyncDurableFault _fault;
	QThread *_thread = nullptr;
	QByteArray _directoryIdentity;
	std::unique_ptr<SyncLocalStore> _local;
	std::optional<SyncCarryJournal> _journal;
	SyncPrivateSnapshot _journalFile;
	SyncPrivateSnapshot _stateFile;
	SyncPrivateSnapshot _cacheFile;
	bool _foreignLogin = false;
	QJsonObject _envelope;
	QJsonObject _objects;
	qsizetype _objectBytes = 0;
	bool _failed = false;
	DesktopSyncChatReadStamp _ioReadOpened;
	std::map<QString, std::pair<uint64_t, DesktopSyncChatReadStamp>> _ioReadFences;
	std::map<QString, std::pair<uint64_t, std::weak_ptr<std::atomic<bool>>>> _ioActive;
	std::shared_ptr<std::atomic<uint64_t>> _ioEpoch = std::make_shared<std::atomic<uint64_t>>(0);

};

}

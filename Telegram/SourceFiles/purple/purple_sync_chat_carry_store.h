#pragma once

#include "purple/purple_sync_chat_journal.h"
#include "purple/purple_sync_local_store.h"

#include <QtCore/QJsonObject>
#include <QtCore/QThread>

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

class SyncChatCarryStore final {
public:
	SyncChatCarryStore(QString directory, SyncChatCarryStoreScope scope,
		SyncDurableFault fault = {});
	~SyncChatCarryStore();
	SyncChatCarryStore(const SyncChatCarryStore &) = delete;
	SyncChatCarryStore &operator=(const SyncChatCarryStore &) = delete;
	[[nodiscard]] SyncChatCarryStorageResult Open();
	void Close();
	[[nodiscard]] SyncChatCarryStorageResult Begin(const SyncCarryJournalStart &input);
	[[nodiscard]] SyncChatCarryStorageResult Advance(SyncCarryJournalEvent event);
	[[nodiscard]] SyncChatCarryStorageResult PrepareState(const SyncLocalState &state);
	[[nodiscard]] SyncChatCarryStorageResult StageOwn();
	[[nodiscard]] SyncChatCarryStorageResult CommitPreparedState();
	[[nodiscard]] SyncChatCarryStorageResult RetireStage();
	[[nodiscard]] SyncChatCarryStorageResult Resume();
	[[nodiscard]] const SyncCarryJournal *journal() const;
	[[nodiscard]] const SyncLocalState *state() const;
	[[nodiscard]] SyncStoreResult OwnBase() const;

private:
	[[nodiscard]] bool Current() const;
	[[nodiscard]] bool StateMatches() const;
	[[nodiscard]] SyncChatCarryStorageResult Fail(SyncStoreStatus status);
	[[nodiscard]] SyncChatCarryStorageResult Persist(const SyncCarryJournalResult &next);
	[[nodiscard]] SyncChatCarryStorageResult StoreObject(const QString &kind, const QByteArray &bytes);
	[[nodiscard]] std::optional<QByteArray> ReadObject(const QString &kind, const QString &hash) const;
	[[nodiscard]] SyncStoreResult FindBase(const SyncLocalState &state) const;
	[[nodiscard]] SyncStoreStatus WitnessState(const QString &hash);
	[[nodiscard]] SyncStoreStatus WitnessCache(const SyncCarryCacheReceipt &receipt);
	[[nodiscard]] SyncChatCarryStorageResult Apply(const SyncCarryJournalEvent &event);
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
	QJsonObject _envelope;
	QJsonObject _objects;
	qsizetype _objectBytes = 0;
	bool _failed = false;

};

}

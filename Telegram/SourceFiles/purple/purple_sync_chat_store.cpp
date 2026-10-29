#include "purple/purple_sync_chat_store.h"

#include "purple/purple_sync_local_store.h"
#include "purple/purple_sync_chat_carry_store.h"
#include <QtCore/QDirIterator>
#include <QtCore/QFileInfo>
#include <QtCore/QDir>
#include <algorithm>

namespace Purple {

std::shared_ptr<DesktopSyncChatDisk> OpenDesktopSyncChatDisk(
	const QString &root,
	const QString &syncDirectory,
	const QString &statePath,
	const AccountKey &account,
	const QByteArray &bindingToken,
	const QString &loginBinding,
	const std::shared_ptr<std::atomic<bool>> &current,
	bool borrowedLock) {
	auto result = std::make_shared<DesktopSyncChatDisk>();
	result->account = account;
	result->loginBinding = loginBinding;
	const auto fail = [&](QString reason) {
		result->error = std::move(reason);
		result->lock.reset();
		return result;
	};
	if (loginBinding.size() != 64 || !current || !current->load()
		|| DesktopSyncChatCacheFileName(account).isEmpty()) {
		return fail(u"origin-retired"_q);
	}
	result->cachePath =
		QDir(root).filePath(DesktopSyncChatCacheFileName(account));
	result->statePath = statePath;
	if (EnsureSyncPrivateDirectory(syncDirectory) != SyncStoreStatus::Ready) {
		return fail(u"sync-directory-held"_q);
	}
	if (!borrowedLock) {
		result->lock = std::make_unique<QLockFile>(syncDirectory + u"/lock"_q);
		result->lock->setStaleLockTime(0);
		if (!result->lock->tryLock(0)) {
			return fail(u"account-lock-busy"_q);
		}
	}
	if (SyncCarryStartupAdmission(syncDirectory, bindingToken) != SyncStoreStatus::Ready) {
		return fail(u"carry-recovery-required"_q);
	}
	auto carryPaths = QStringList{syncDirectory + u"/carry.json"_q};
	const auto historyDirectory = syncDirectory + u"/carry-history"_q;
	if (SyncPathIdentity(historyDirectory) != "missing" && (QFileInfo(historyDirectory).isSymLink() || !QFileInfo(historyDirectory).isDir())) { return fail(u"carry-history-held"_q); }
	auto history = QDirIterator(historyDirectory, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
	while (history.hasNext()) {
		carryPaths.push_back(history.next());
		if (carryPaths.size() > 65) { return fail(u"carry-retention-full"_q); }
	}
	auto retainedBytes = qint64(0);
	for (const auto &path : carryPaths) {
		const auto snapshot = CaptureSyncPrivateFile(path, kSyncCarryJournalStorageLimit);
		if (snapshot.status != SyncStoreStatus::Ready) { return fail(u"carry-history-held"_q); }
		if (snapshot.absent()) { continue; }
		const auto journal = InspectSyncCarryJournal(snapshot.bytes, bindingToken);
		if (!journal || journal->account != account || SyncCarryJournalHoldsWrites(*journal)) { return fail(u"carry-history-held"_q); }
		retainedBytes += snapshot.bytes.size();
		if (retainedBytes > kSyncCarryObjectStorageLimit
			|| (path != syncDirectory + u"/carry.json"_q
				&& QFileInfo(path).fileName() != SyncCarryRecordHash(snapshot.bytes) + u".json"_q)) { return fail(u"carry-history-held"_q); }
		const auto existing = std::find_if(result->retainedCarryEffects.begin(), result->retainedCarryEffects.end(), [&](const auto &saved) { return saved.operationId == journal->operationId; });
		if (existing != result->retainedCarryEffects.end()) {
			if (result->retainedCarryImages[size_t(existing - result->retainedCarryEffects.begin())] != snapshot.bytes) { return fail(u"carry-history-conflict"_q); }
		} else { result->retainedCarryEffects.push_back(*journal); result->retainedCarryImages.push_back(snapshot.bytes); }
	}
	result->identity = SyncPathIdentity(result->cachePath);
	if (result->identity.isEmpty()) {
		return fail(u"cache-path-held"_q);
	} else if (result->identity != "missing") {
		if (ReadSyncPrivateFile(result->cachePath, 64 * 1024, result->bytes)
			!= SyncStoreStatus::Ready) {
			return fail(u"cache-read-held"_q);
		}
		const auto parsed = ParseDesktopSyncChatCache(result->bytes, account);
		if (!parsed.record) {
			return fail(
				parsed.status == DesktopSyncChatCacheStatus::Newer
					? u"newer-cache"_q
					: u"invalid-cache"_q);
		}
		if (parsed.record->loginBinding != loginBinding) {
			if (result->retainedCarryEffects.empty()) { return fail(u"cache-login-held"_q); }
		} else {
			result->cache = parsed.record;
		}
	}
	result->stateIdentity = SyncPathIdentity(statePath);
	if (result->stateIdentity.isEmpty()) {
		return fail(u"state-path-held"_q);
	} else if (result->stateIdentity != "missing") {
		auto bytes = QByteArray();
		if (ReadSyncPrivateFile(statePath, 1024 * 1024, bytes)
			!= SyncStoreStatus::Ready) {
			return fail(u"state-read-held"_q);
		}
		result->stateBytes = bytes;
		const auto state = ParseSyncLocalState(bytes);
		if (!state
			|| CheckSyncAccountBinding(state.state, bindingToken)
				!= SyncAccountBindingVerdict::Bound) {
			return fail(u"state-binding-held"_q);
		}
		result->settingsBound = true;
		result->settingsChat = state.state.chat;
		result->settingsSpace = state.state.space;
	}
	if (!current->load()
		|| SyncPathIdentity(result->cachePath) != result->identity
		|| SyncPathIdentity(statePath) != result->stateIdentity) {
		return fail(u"opening-changed"_q);
	}
	return result;
}

bool SaveDesktopSyncChatDisk(
	const std::shared_ptr<DesktopSyncChatDisk> &disk,
	const std::optional<DesktopSyncChatCacheRecord> &record,
	const std::shared_ptr<std::atomic<bool>> &current) {
	if (!disk
		|| (record
			&& (record->account != disk->account
				|| record->loginBinding != disk->loginBinding))
		|| !disk->error.isEmpty() || !current || !current->load()
		|| SyncPathIdentity(disk->cachePath) != disk->identity
		|| SyncPathIdentity(disk->statePath) != disk->stateIdentity) {
		return false;
	}
	if (disk->stateIdentity != "missing") {
		auto stateBytes = QByteArray();
		if (ReadSyncPrivateFile(disk->statePath, 1024 * 1024, stateBytes)
				!= SyncStoreStatus::Ready
			|| stateBytes != disk->stateBytes) {
			return false;
		}
	}
	if (disk->identity != "missing") {
		auto before = QByteArray();
		if (ReadSyncPrivateFile(disk->cachePath, 64 * 1024, before)
				!= SyncStoreStatus::Ready
			|| before != disk->bytes) {
			return false;
		}
	}
	if (!record) {
		return true;
	}
	const auto bytes = SerializeDesktopSyncChatCache(*record);
	if (!bytes) {
		return false;
	} else if (*bytes == disk->bytes) {
		return current->load();
	}

	return current->load()
		&& WriteSyncPrivateFile(disk->cachePath, *bytes)
		== SyncStoreStatus::Ready
		&& current->load();
}

}

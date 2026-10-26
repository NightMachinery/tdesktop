#include "purple/purple_sync_chat_store.h"

#include "purple/purple_sync_local_store.h"
#include <QtCore/QDir>

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
			return fail(u"cache-login-held"_q);
		}
		result->cache = parsed.record;
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

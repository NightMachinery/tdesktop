#pragma once

#include "purple/purple_sync_chat_cache.h"

#include <QtCore/QLockFile>
#include <atomic>
#include <memory>

namespace Purple {

struct DesktopSyncChatDisk {
	AccountKey account;
	QString loginBinding;
	QString cachePath;
	QString statePath;
	QByteArray identity;
	QByteArray stateIdentity;
	QByteArray stateBytes;
	QByteArray bytes;
	std::optional<DesktopSyncChatCacheRecord> cache;
	std::unique_ptr<QLockFile> lock;
	bool settingsBound = false;
	PeerToken settingsChat;
	QString settingsSpace;
	QString error;
};

[[nodiscard]] std::shared_ptr<DesktopSyncChatDisk> OpenDesktopSyncChatDisk(
	const QString &root,
	const QString &syncDirectory,
	const QString &statePath,
	const AccountKey &account,
	const QByteArray &bindingToken,
	const QString &loginBinding,
	const std::shared_ptr<std::atomic<bool>> &current,
	bool borrowedLock = false);
[[nodiscard]] bool SaveDesktopSyncChatDisk(
	const std::shared_ptr<DesktopSyncChatDisk> &disk,
	const std::optional<DesktopSyncChatCacheRecord> &record,
	const std::shared_ptr<std::atomic<bool>> &current);

}

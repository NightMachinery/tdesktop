#pragma once

#include "purple/purple_sync_chat_facts.h"

#include <QtCore/QJsonObject>

namespace Purple {

struct DesktopSyncChatCacheRecord {
	AccountKey account;
	SyncChatCache mark;
	QString title;
	SyncChatReaderFacts readers;
	bool readerConsent = false;
	std::optional<QJsonObject> carry;
	QJsonObject preserved;
	QString loginBinding;
};

enum class DesktopSyncChatCacheStatus { Valid, Invalid, Newer };

struct DesktopSyncChatCacheParse {
	DesktopSyncChatCacheStatus status = DesktopSyncChatCacheStatus::Invalid;
	std::optional<DesktopSyncChatCacheRecord> record;
};

[[nodiscard]] DesktopSyncChatCacheParse
ParseDesktopSyncChatCache(const QByteArray &bytes, const AccountKey &account);
[[nodiscard]] std::optional<QByteArray> SerializeDesktopSyncChatCache(const DesktopSyncChatCacheRecord &record);
[[nodiscard]] QString DesktopSyncChatCacheFileName(const AccountKey &account);
[[nodiscard]] bool DesktopSyncChatCacheCanCommit(
	const std::optional<DesktopSyncChatCacheRecord> &previous,
	const SyncChatResolveInput &input,
	const SyncChatResolution &resolution,
	bool completeDiscovery,
	bool current);

}

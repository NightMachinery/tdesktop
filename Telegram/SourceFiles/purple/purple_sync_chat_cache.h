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
	bool paused = false;
	std::vector<QString> disclosedStreams;
	std::optional<QJsonObject> carry;
	QJsonObject preserved;
	QString loginBinding;
	bool newReaders = false;
	std::optional<bool> readerCreator;
	std::optional<std::vector<QString>> readerMembers;
};

struct DesktopSyncChatDiscoveryScope {
	DesktopSyncChatOrigin origin;
	QString root;
	QByteArray binding;
	QString loginBinding;
	std::optional<AccountKey> owner;
	AccountWorkMode mode = AccountWorkMode::Own;
	QString space;
	bool settingsBound = false;
	PeerToken settingsChat;
	std::optional<SyncChatCache> mark;
	bool carry = false;

	[[nodiscard]] bool operator==(const DesktopSyncChatDiscoveryScope &) const = default;
};

struct DesktopSyncChatDiscoveryReceipt {
	DesktopSyncChatDiscoveryScope scope;
	int64_t completedAt = 0;
};

[[nodiscard]] bool ReuseDesktopSyncChatDiscovery(
	const DesktopSyncChatDiscoveryReceipt &receipt,
	const DesktopSyncChatDiscoveryScope &current, int64_t now);
[[nodiscard]] bool NewDesktopSyncChatReaders(
	const DesktopSyncChatCacheRecord &previous, const DesktopSyncChatPeer &peer);

enum class DesktopSyncChatCacheStatus { Valid, Invalid, Newer };

struct DesktopSyncChatCacheParse {
	DesktopSyncChatCacheStatus status = DesktopSyncChatCacheStatus::Invalid;
	std::optional<DesktopSyncChatCacheRecord> record;
	bool paused = false;
	bool pauseOnly = false;
};

[[nodiscard]] DesktopSyncChatCacheParse
ParseDesktopSyncChatCache(const QByteArray &bytes, const AccountKey &account);
[[nodiscard]] std::optional<QByteArray> SerializeDesktopSyncChatCache(const DesktopSyncChatCacheRecord &record);
[[nodiscard]] std::optional<QByteArray> DesktopSyncChatPauseImage(
    const QByteArray &opening, const AccountKey &account, const QString &login,
    bool paused, bool foreground);
[[nodiscard]] std::optional<QByteArray> DesktopSyncChatForgetLoginImage(
    const QByteArray &opening, const AccountKey &account, const QString &login);
[[nodiscard]] QString DesktopSyncChatCacheFileName(const AccountKey &account);
[[nodiscard]] bool DesktopSyncChatCacheCanCommit(
	const std::optional<DesktopSyncChatCacheRecord> &previous,
	const SyncChatResolveInput &input,
	const SyncChatResolution &resolution,
	bool completeDiscovery,
	bool current);

}

#pragma once

#include "purple/purple_archive_sqlite.h"
#include "purple/purple_archive_views.h"
#include <array>
#include <atomic>
#include <chrono>

namespace Purple {

enum class ArchiveQueryStatus { Ready, Stale, Busy, Unavailable, SchemaMismatch, ReadOnly };

struct ArchiveDeletedEntry {
	ArchiveMessage message;
	QJsonObject shellMetadata;
	QJsonObject headMetadata;
	ArchiveContent displayContent;
	QString contentHash;
	QString markdownDigest;
	std::optional<ArchiveVersionSnapshot> head;
	ArchiveGeneration generation;
	ArchiveTranscriptCursor cursor;
};

struct ArchiveDeletedPage {
	ArchiveQueryStatus status = ArchiveQueryStatus::Unavailable;
	QString view;
	QString reason;
	ArchiveStoreEpoch epoch;
	ArchiveState header;
	std::shared_ptr<std::atomic<bool>> active;
	std::array<int64, 7> counts = {};
	std::vector<ArchiveDeletedEntry> entries;
	uint64_t logicalBytes = 0;
	bool logicalComplete = false;
	bool more = false;
	bool refreshAvailable = false;
	bool hasGap = false;
};

[[nodiscard]] std::optional<ArchiveStoredRow> DesktopArchiveDecodeRow(
	DesktopArchiveSqlite &database, const AccountKey &account,
	const ArchiveSqlSecurity &security, ArchiveRecordKind kind,
	const QString &id, const QByteArray &frame, int64 position = 0);
[[nodiscard]] ArchiveSqlStatus DesktopArchiveAdmitSnapshot(DesktopArchiveSqlite &database);
[[nodiscard]] std::optional<ArchiveStoredRow> DesktopArchiveReadHeader(
	DesktopArchiveSqlite &database, const AccountKey &account,
	const ArchiveSqlSecurity &security, const QString &maintenance = {},
	bool *readFailure = nullptr);
[[nodiscard]] std::optional<uint64_t> DesktopArchiveReadChatGeneration(
	DesktopArchiveSqlite &database, const AccountKey &account,
	const ArchiveChatKey &chat, const ArchiveSqlSecurity &security);

class DesktopArchiveDeletedQuery final {
public:
	DesktopArchiveDeletedQuery(QString id, ArchiveStoreEpoch epoch,
		ArchiveSqlSecurity security, ArchiveViewScope scope, ArchiveTranscriptOrder order);
	[[nodiscard]] ArchiveQueryStatus Open(const QString &path, std::function<bool()> active);
	[[nodiscard]] ArchiveDeletedPage Read(int limit);
	[[nodiscard]] bool Current(DesktopArchiveSqlite &writer);
	[[nodiscard]] bool Expired() const;
	[[nodiscard]] bool Close();
	[[nodiscard]] const ArchiveStoreEpoch &epoch() const;

private:
	[[nodiscard]] bool ReadUsage();
	[[nodiscard]] std::optional<ArchiveDeletedEntry> DecodeEntry(
		const std::vector<ArchiveSqlValue> &values);
	DesktopArchiveSqlite _database;
	std::shared_ptr<std::atomic<bool>> _active = std::make_shared<std::atomic<bool>>(true);
	bool _refreshAvailable = false;
	QString _id;
	ArchiveStoreEpoch _epoch;
	ArchiveSqlSecurity _security;
	ArchiveViewScope _scope;
	ArchiveTranscriptOrder _order;
	ArchiveState _header;
	std::array<int64, 7> _counts = {};
	std::vector<ArchiveChatGeneration> _generations;
	std::optional<ArchiveTranscriptCursor> _cursor;
	QString _blobCursor;
	uint64_t _logicalBytes = 0;
	bool _logicalComplete = false;
	bool _more = true;
	std::chrono::steady_clock::time_point _lastUse;

};

}

#include "purple/purple_archive_v2_store.h"

#include "purple/purple_archive_v2_media.h"

#include <QtCore/QDateTime>
#include <QtCore/QFileInfo>
#include <QtCore/QStorageInfo>
#include <algorithm>
#include <limits>

namespace Purple::ArchiveV2 {
namespace {

constexpr auto kBatch = int64(64);
constexpr auto kRows = ArchiveSqlExpectation::Rows;
constexpr auto kRow = ArchiveSqlExpectation::OneRow;
constexpr auto kChanges = ArchiveSqlExpectation::AnyChanges;

std::optional<int64> Integer(const DesktopArchiveSqlResult &read, size_t column = 0) {
	if (!read.ok || read.rows.size() != 1 || read.rows.front().size() <= column) return std::nullopt;
	const auto value = std::get_if<int64>(&read.rows.front()[column]);
	return value && *value >= 0 ? std::optional(*value) : std::nullopt;
}

bool Valid(const Policy &policy) {
	return policy.valid && policy.days >= 1 && policy.days <= 36500
		&& policy.totalMiB >= 16 && policy.totalMiB <= 65536
		&& policy.timedMiB >= 0 && policy.timedMiB <= 65536
		&& policy.ordinaryFileMiB >= 1 && policy.ordinaryFileMiB <= policy.totalMiB
		&& std::all_of(policy.classDays.begin(), policy.classDays.end(), [](const auto &days) {
			return !days || (*days >= 1 && *days <= 36500);
		});
}

QString Failure(const DesktopArchiveSqlResult &result) {
	return result.diskFull() ? u"disk_full"_q : u"quota_unreclaimable"_q;
}

}

std::optional<int64> Store::pragma(const QString &query) {
	return Integer(_db.Read({ query, {}, kRow }));
}

int64 Store::availableBytes() const {
	return _options.availableBytes ? _options.availableBytes() : QStorageInfo(directory()).bytesAvailable();
}

int64 Store::today() const {
	return std::max<int64>(0, _options.now ? _options.now() : QDateTime::currentSecsSinceEpoch()) / 86400;
}

std::optional<Usage> Store::usage() {
	if (!current()) return std::nullopt;
	const auto path = directory() + u"/archive.sqlite"_q;
	const auto media = _db.Read({ u"SELECT "
		"coalesce(sum(CASE WHEN timed=0 THEN size ELSE 0 END),0),"
		"coalesce(sum(CASE WHEN timed=1 THEN size ELSE 0 END),0) "
		"FROM (SELECT b.size,max(m.timed) timed FROM blobs b JOIN media m USING(blob_key) GROUP BY b.blob_key)"_q, {}, kRow });
	const auto ordinary = Integer(media, 0);
	const auto timed = Integer(media, 1);
	const auto metadata = Integer(_db.Read({ u"SELECT "
		"coalesce((SELECT sum(length(sealed_identity)+160) FROM messages),0)+"
		"coalesce((SELECT sum(length(sealed_body)+144) FROM versions),0)+"
		"coalesce((SELECT sum(length(sealed_descriptor)+144) FROM media),0)+"
		"coalesce((SELECT count(*)*128 FROM purged_edits),0)"_q, {}, kRow }));
	if (!ordinary || !timed || !metadata || *ordinary > std::numeric_limits<int64>::max() - *metadata) return std::nullopt;
	return Usage{ QFileInfo(path).size(), QFileInfo(path + u"-wal"_q).size(), QFileInfo(path + u"-shm"_q).size(),
		QFileInfo(path + u"-journal"_q).size() + QFileInfo(pointerPath()).size()
			+ QFileInfo(_root + u"/live.lock"_q).size(), *ordinary + *metadata, *timed, _reclaimed };
}

QString Store::physicalLimit(int64 extra) {
	const auto &policy = _options.policy;
	if (!Valid(policy) || extra < 0) return u"quota_unreclaimable"_q;
	const auto page = pragma(u"PRAGMA page_size"_q);
	const auto count = pragma(u"PRAGMA page_count"_q);
	const auto free = pragma(u"PRAGMA freelist_count"_q);
	if (!page || *page < 512 || !count || !free || *free > *count) return u"store_unreadable"_q;
	const auto available = availableBytes();
	const auto path = directory() + u"/archive.sqlite"_q;
	const auto wal = QFileInfo(path + u"-wal"_q).size();
	if (available < wal + 128 * 1024) return u"disk_full"_q;
	if (wal) {
		const auto result = _db.CheckpointV2();
		if (!result.ok) return result.diskFull() ? u"disk_full"_q : u"store_busy"_q;
	}
	const auto cap = (int64(policy.totalMiB) + policy.timedMiB) * kMiB;
	const auto support = QFileInfo(path + u"-shm"_q).size() + QFileInfo(path + u"-journal"_q).size()
		+ QFileInfo(pointerPath()).size() + QFileInfo(_root + u"/live.lock"_q).size();
	const auto margin = support + std::max<int64>(256 * 1024, cap / 100);
	const auto maximum = (cap - margin) / (2 * *page + 24);
	const auto required = std::max(*count, *count - *free + (extra + *page - 1) / *page);
	if (required > maximum) return u"quota_unreclaimable"_q;
	const auto activeBytes = QFileInfo(path).size() + QFileInfo(path + u"-shm"_q).size();
	const auto availableNow = std::min(cap, availableBytes());
	const auto diskMaximum = (availableNow + activeBytes - margin) / (2 * *page + 24);
	if (required > diskMaximum) return u"disk_full"_q;
	const auto selected = std::min(maximum, diskMaximum);
	const auto limit = pragma(u"PRAGMA max_page_count=%1"_q.arg(selected));
	return limit == selected ? QString() : u"quota_unreclaimable"_q;
}

QString Store::expire(int64 day, const QByteArray &keep) {
	const auto &policy = _options.policy;
	const auto result = _db.Execute({
		ArchiveSqlStatus::Ready, {
			{ u"BEGIN IMMEDIATE"_q },
			{ u"DELETE FROM messages WHERE message_key IN (SELECT message_key FROM messages "
				"WHERE message_key<>? AND last_activity<=?-CASE class WHEN 0 THEN ? WHEN 1 THEN ? WHEN 2 THEN ? "
				"WHEN 3 THEN ? ELSE ? END ORDER BY last_activity,message_key LIMIT ?)"_q,
				{ keep, day, int64(policy.daysFor(ChatClass::Direct)), int64(policy.daysFor(ChatClass::Bot)),
					int64(policy.daysFor(ChatClass::Group)), int64(policy.daysFor(ChatClass::Channel)),
					int64(policy.days), kBatch }, kChanges },
			{ u"COMMIT"_q },
		},
	}, [this] { return current(); });
	return result.ok ? QString() : Failure(result);
}

QString Store::trim(int64 bytes, bool timed, const QByteArray &keep, const QByteArray &keepBlob, bool forceOne) {
	const auto cap = int64(timed ? _options.policy.timedMiB : _options.policy.totalMiB) * kMiB;
	if (bytes > cap) return timed ? u"over_allowance"_q : u"quota_unreclaimable"_q;
	for (auto i = int64(0); i <= kBatch; ++i) {
		const auto size = usage();
		if (!size) return u"store_unreadable"_q;
		if (forceOne ? i > 0 : (timed ? size->timed : size->ordinary) <= cap - bytes) return {};
		if (i == kBatch) break;
		const auto oldest = _db.Read({ u"SELECT b.blob_key FROM blobs b JOIN media r USING(blob_key) "
			"JOIN messages m ON m.message_key=r.message_key WHERE b.blob_key<>? AND m.message_key<>? "
			"GROUP BY b.blob_key HAVING max(r.timed)=? "
			"AND NOT EXISTS(SELECT 1 FROM media retained WHERE retained.blob_key=b.blob_key "
				"AND (retained.message_key=? OR retained.timed>?)) "
			"ORDER BY min(m.last_activity),b.blob_key LIMIT 1"_q,
			{ keepBlob, keep, int64(timed), keep, int64(timed) }, kRows });
		if (!oldest.ok) return u"store_unreadable"_q;
		auto plan = ArchiveSqlProgram{ ArchiveSqlStatus::Ready, {{ u"BEGIN IMMEDIATE"_q }} };
		if (!oldest.rows.empty()) {
			plan.statements.push_back({ u"UPDATE media SET blob_key=NULL,sealed_blob_ref=NULL,state=0,reason=1 WHERE blob_key=?"_q,
				{ oldest.rows.front().front() }, kChanges });
		} else if (!timed) {
			const auto history = _db.Read({ u"SELECT m.message_key FROM messages m WHERE m.message_key<>? "
				"AND NOT EXISTS(SELECT 1 FROM media own JOIN media other ON own.blob_key=other.blob_key "
					"WHERE own.message_key=m.message_key AND (other.timed=1 OR other.blob_key=?)) "
				"ORDER BY m.last_activity,m.message_key LIMIT 1"_q, { keep, keepBlob }, kRows });
			if (!history.ok) return u"store_unreadable"_q;
			if (history.rows.empty()) break;
			plan.statements.push_back({ u"DELETE FROM messages WHERE message_key=?"_q, { history.rows.front().front() }, kChanges });
		} else {
			break;
		}
		plan.statements.push_back({ u"COMMIT"_q });
		const auto checkpoint = _db.CheckpointV2();
		if (!checkpoint.ok) return checkpoint.diskFull() ? u"disk_full"_q : u"store_busy"_q;
		const auto result = _db.Execute(plan, [this] { return current(); });
		if (!result.ok) return Failure(result);
	}
	return u"quota_unreclaimable"_q;
}

QString Store::reserve(int64 logical, int64 physical, bool timed,
		const QByteArray &keep, const QByteArray &keepBlob) {
	if (!current()) return u"scope_retired"_q;
	if (const auto failure = physicalLimit(0); !failure.isEmpty()) return failure;
	const auto day = today();
	if (_lastTrimDay != day) {
		if (const auto failure = expire(day, keep); !failure.isEmpty()) return failure;
		_lastTrimDay = day;
	}
	if (const auto failure = trim(logical, timed, keep, keepBlob); !failure.isEmpty()) return failure;
	for (auto i = int64(0); i != kBatch; ++i) {
		const auto failure = physicalLimit(physical + 64 * 1024);
		if (failure != u"quota_unreclaimable"_q) return failure;
		if (const auto evicted = trim(0, timed, keep, keepBlob, true); !evicted.isEmpty()) return evicted;
	}
	return u"quota_unreclaimable"_q;
}

StoreEffect Store::maintain() {
	if (!current()) return { u"scope_retired"_q };
	if (!Valid(_options.policy)) return { u"policy_invalid"_q };
	const auto before = usage();
	if (!before) return { u"store_unreadable"_q };
	const auto physical = physicalLimit(0);
	if (physical == u"disk_full"_q || physical == u"store_busy"_q) return { physical };
	const auto day = today();
	if (const auto failure = expire(day); !failure.isEmpty()) return { failure };
	_lastTrimDay = day;
	if (const auto failure = trim(0, false, {}); !failure.isEmpty()) return { failure };
	if (const auto failure = trim(0, true, {}); !failure.isEmpty()) return { failure };
	if (physical == u"quota_unreclaimable"_q) {
		if (const auto failure = trim(0, false, {}, {}, true); !failure.isEmpty()
			&& failure != u"quota_unreclaimable"_q) return { failure };
	}
	const auto checkpoint = _db.CheckpointV2();
	if (!checkpoint.ok) return { Failure(checkpoint) };
	const auto freed = pragma(u"PRAGMA freelist_count"_q);
	if (freed && *freed > 0) {
		const auto vacuum = _db.Read({ u"PRAGMA incremental_vacuum(64)"_q, {}, kRows });
		if (!vacuum.ok) return { Failure(vacuum) };
		const auto flushed = _db.CheckpointV2();
		if (!flushed.ok) return { Failure(flushed) };
	}
	const auto after = usage();
	if (!after) return { u"store_unreadable"_q };
	_reclaimed += std::max<int64>(0, before->physical() - after->physical());
	return { physicalLimit(0) };
}

std::optional<int64> Store::estimateExpired(const Policy &policy) {
	if (!current() || !Valid(policy)) return std::nullopt;
	return Integer(_db.Read({ u"SELECT count(*) FROM messages WHERE last_activity<=?-CASE class "
		"WHEN 0 THEN ? WHEN 1 THEN ? WHEN 2 THEN ? WHEN 3 THEN ? ELSE ? END"_q,
		{ today(), int64(policy.daysFor(ChatClass::Direct)), int64(policy.daysFor(ChatClass::Bot)),
			int64(policy.daysFor(ChatClass::Group)), int64(policy.daysFor(ChatClass::Channel)),
			int64(policy.days) }, kRow }));
}

StoreEffect Store::updatePolicy(Policy policy) {
	if (!Valid(policy)) return { u"policy_invalid"_q };
	_options.policy = std::move(policy);
	return maintain();
}

}

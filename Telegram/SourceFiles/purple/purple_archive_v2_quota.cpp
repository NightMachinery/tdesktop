#include "purple/purple_archive_v2_store.h"

#include "purple/purple_archive_v2_media.h"

#include <QtCore/QDateTime>
#include <QtCore/QFileInfo>
#include <QtCore/QStorageInfo>
#include <algorithm>

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
	return result.diskFull() ? u"disk_full"_q : u"store_unreadable"_q;
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

std::optional<std::pair<int64, int64>> Store::totals(bool budget) {
	const auto read = _db.Read(budget ? ArchiveSqlStatement{
		u"SELECT ordinary-min(ordinary_framing,?),timed-min(timed_framing,?) FROM meta"_q,
		{ kBlobFramingCredit, kBlobFramingCredit }, kRow
	} : ArchiveSqlStatement{ u"SELECT ordinary,timed FROM meta"_q, {}, kRow });
	const auto ordinary = Integer(read);
	const auto timed = Integer(read, 1);
	return ordinary && timed ? std::optional(std::pair(*ordinary, *timed)) : std::nullopt;
}

std::optional<Usage> Store::usage() {
	if (!current()) return std::nullopt;
	const auto size = totals();
	if (!size) return std::nullopt;
	const auto path = directory() + u"/archive.sqlite"_q;
	return Usage{ QFileInfo(path).size(), QFileInfo(path + u"-wal"_q).size(), QFileInfo(path + u"-shm"_q).size(),
		QFileInfo(path + u"-journal"_q).size() + QFileInfo(pointerPath()).size()
			+ QFileInfo(_root + u"/live.lock"_q).size(), size->first, size->second };
}

QString Store::physicalLimit(int64 extra, bool reclaiming, int64 *freePages) {
	const auto &policy = _options.policy;
	if (!Valid(policy)) return u"policy_invalid"_q;
	const auto page = pragma(u"PRAGMA page_size"_q);
	const auto count = pragma(u"PRAGMA page_count"_q);
	const auto free = pragma(u"PRAGMA freelist_count"_q);
	if (!page || *page < 512 || !count || !free || *free > *count) return u"store_unreadable"_q;
	const auto frames = (extra + *page - 1) / *page + 32;
	const auto budget = frames * (*page + 24);
	const auto wal = QFileInfo(directory() + u"/archive.sqlite-wal"_q).size();
	if (budget + wal + 128 * 1024 > availableBytes()) return u"disk_full"_q;
	if (budget > kWalHeadroom) return u"quota_unreclaimable"_q;
	if (wal + budget > kWalHeadroom) {
		const auto result = _db.CheckpointV2();
		if (!result.ok) return result.diskFull() ? u"disk_full"_q : u"store_busy"_q;
	}
	const auto desired = ((int64(policy.totalMiB) + policy.timedMiB) * kMiB + kMetadataHeadroom - kMiB) / *page;
	// A reduction may leave the existing database above the new ceiling until vacuum runs.
	const auto maximum = std::max(desired, *count);
	if (!reclaiming && *count - *free + frames > maximum) {
		if (freePages) *freePages = frames - (maximum - *count);
		return u"quota_unreclaimable"_q;
	}
	if (_pageLimit != maximum) {
		const auto limit = pragma(u"PRAGMA max_page_count=%1"_q.arg(maximum));
		if (limit != maximum) return u"quota_unreclaimable"_q;
		_pageLimit = maximum;
	}
	return {};
}

QString Store::reservePages(int64 bytes, bool timed, const QByteArray &keep, const QByteArray &blob) {
	auto freePages = int64(0);
	const auto failure = physicalLimit(bytes, false, &freePages);
	if (!freePages) return failure;
	auto trimmed = QString();
	do {
		trimmed = trim(0, timed, keep, blob, 0, freePages);
	} while (trimmed == u"trim_pending"_q && current());
	return trimmed.isEmpty() ? physicalLimit(bytes) : trimmed;
}

QString Store::expire(int64 day, const QByteArray &keep) {
	const auto &policy = _options.policy;
	const auto result = _db.Execute({
		ArchiveSqlStatus::Ready, {
			{ u"BEGIN IMMEDIATE"_q },
			{ u"DELETE FROM messages WHERE message_key IN (SELECT message_key FROM messages "
				"WHERE message_key<>? AND message_key<>? AND last_activity<=?-CASE class WHEN 0 THEN ? WHEN 1 THEN ? WHEN 2 THEN ? "
				"WHEN 3 THEN ? ELSE ? END ORDER BY last_activity,message_key LIMIT ?)"_q,
				{ keep, _mediaKeep, day, int64(policy.daysFor(ChatClass::Direct)), int64(policy.daysFor(ChatClass::Bot)),
					int64(policy.daysFor(ChatClass::Group)), int64(policy.daysFor(ChatClass::Channel)),
					int64(policy.days), kBatch }, kChanges },
			{ u"COMMIT"_q },
		},
	}, [this] { return current(); });
	if (!result.ok) return Failure(result);
	const auto removed = Integer(_db.Read({ u"SELECT changes()"_q, {}, kRow }));
	if (!removed) return u"store_unreadable"_q;
	if (*removed < kBatch) _lastTrimDay = day;
	_vacuumPending |= *removed > 0;
	return {};
}

QString Store::trim(int64 bytes, bool timed, const QByteArray &keep, const QByteArray &keepBlob,
		int64 framing, int64 freePages) {
	const auto cap = int64(timed ? _options.policy.timedMiB : _options.policy.totalMiB) * kMiB;
	if (bytes > cap) return u"over_allowance"_q;
	const auto bucket = timed ? u"timed"_q : u"ordinary"_q;
	const auto query = ArchiveSqlStatement{
		u"SELECT %1+?+?-min(%1_framing+?,?) FROM meta"_q.arg(bucket), { bytes, framing, framing, kBlobFramingCredit }, kRow
	};
	const auto fits = [&]() -> std::optional<bool> {
		const auto charge = Integer(_db.Read(query));
		const auto free = freePages ? pragma(u"PRAGMA freelist_count"_q) : std::optional<int64>(0);
		return charge && free ? std::optional(*charge <= cap && *free >= freePages) : std::nullopt;
	};
	const auto initial = fits();
	if (!initial) return u"store_unreadable"_q;
	if (*initial) return {};
	if (const auto failure = physicalLimit(2 * kMiB, true); !failure.isEmpty()) return failure;
	auto failure = QString();
	auto removed = false;
	const auto result = _db.ExecuteV2([&](const DesktopArchiveSqlite::Write &put) {
		for (auto i = int64(0); i <= kBatch; ++i) {
			const auto fitsNow = fits();
			if (!fitsNow) return false;
			if (*fitsNow) return true;
			if (i == kBatch) { failure = u"trim_pending"_q; return true; }
			const auto oldest = _db.Read({ u"SELECT b.blob_key FROM blobs b JOIN media r USING(blob_key) "
				"JOIN messages m USING(message_key) WHERE b.timed=? AND b.sealed_manifest IS NOT NULL AND b.blob_key<>? "
				"GROUP BY b.blob_key HAVING NOT EXISTS(SELECT 1 FROM media retained WHERE retained.blob_key=b.blob_key "
					"AND (retained.message_key=? OR retained.message_key=?)) "
				"ORDER BY min(m.last_activity),b.blob_key LIMIT 1"_q,
				{ int64(timed), keepBlob, keep, _mediaKeep }, kRows });
			if (!oldest.ok) return false;
			auto statement = ArchiveSqlStatement();
			if (!oldest.rows.empty()) {
				statement = { u"UPDATE media SET blob_key=NULL,sealed_blob_ref=NULL,state=0,reason=1 WHERE blob_key=?"_q,
					{ oldest.rows.front().front() }, kChanges };
			} else if (!timed) {
				const auto history = _db.Read({ u"SELECT m.message_key FROM messages m WHERE m.message_key<>? AND m.message_key<>? "
					"AND NOT EXISTS(SELECT 1 FROM media own JOIN blobs b USING(blob_key) "
						"WHERE own.message_key=m.message_key AND (b.timed=1 OR b.blob_key=?)) "
					"ORDER BY m.last_activity,m.message_key LIMIT 1"_q, { keep, _mediaKeep, keepBlob }, kRows });
				if (!history.ok) return false;
				if (history.rows.empty()) { failure = u"quota_unreclaimable"_q; return true; }
				statement = { u"DELETE FROM messages WHERE message_key=?"_q, { history.rows.front().front() }, kChanges };
			} else {
				failure = u"quota_unreclaimable"_q;
				return true;
			}
			if (!put(statement)) return false;
			removed = true;
		}
		return true;
	}, [this] { return current(); });
	if (!result.committed) return Failure(result);
	_vacuumPending |= removed;
	return failure;
}

QString Store::reserve(int64 logical, int64 framing, bool timed, const QByteArray &keep,
		const QByteArray &keepBlob, const std::function<void()> &betweenParts) {
	const auto day = today();
	while (current() && _lastTrimDay != day) {
		if (const auto failure = physicalLimit(kMiB, true); !failure.isEmpty()) return failure;
		if (const auto failure = expire(day, keep); !failure.isEmpty()) return failure;
		if (betweenParts) betweenParts();
	}
	while (current()) {
		const auto result = trim(logical, timed, keep, keepBlob, framing);
		if (result != u"trim_pending"_q) return result;
		if (betweenParts) betweenParts();
	}
	return u"scope_retired"_q;
}

bool Store::maintenancePending() const {
	return current() && (!_pointer.retired.empty() || _trimPending || _vacuumPending || _scrubPending || _lastTrimDay != today());
}

StoreEffect Store::maintain() {
	if (!current()) return { u"scope_retired"_q };
	if (!_pointer.retired.empty()) {
		const auto pending = cleanup();
		if (!pending.isEmpty()) return { pending };
	}
	if (_scrubPending) return scrubParts(false);
	if (!Valid(_options.policy)) return { u"policy_invalid"_q };
	const auto day = today();
	if (_lastTrimDay != day) {
		if (const auto failure = physicalLimit(kMiB, true); !failure.isEmpty()) return { failure };
		if (const auto failure = expire(day); !failure.isEmpty()) return { failure };
	}
	if (_trimPending) {
		const auto timed = trim(0, true, {});
		const auto ordinary = trim(0, false, {});
		_trimPending = timed == u"trim_pending"_q || ordinary == u"trim_pending"_q;
		for (const auto &result : { timed, ordinary }) {
			if (!result.isEmpty() && result != u"trim_pending"_q) {
				_trimPending = true;
				return { result };
			}
		}
	}
	if (_vacuumPending && !_trimPending) {
		const auto before = pragma(u"PRAGMA freelist_count"_q);
		if (!before) return { u"store_unreadable"_q };
		if (*before) {
			if (const auto failure = physicalLimit(kMiB, true); !failure.isEmpty()) return { failure };
			const auto vacuum = _db.Read({ u"PRAGMA incremental_vacuum(128)"_q, {}, kRows });
			if (!vacuum.ok) return { Failure(vacuum) };
		}
		const auto after = pragma(u"PRAGMA freelist_count"_q);
		_vacuumPending = after && *after && *after < *before;
	}
	return {};
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
	_lastTrimDay = -1;
	_trimPending = true;
	return _mediaKeep.isEmpty() ? maintain() : StoreEffect{};
}

}

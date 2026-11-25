#include "purple/purple_music_index.h"

#include "purple/purple_exact_search.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>

namespace Purple {
namespace {

constexpr auto kPageSize = int64(100);
static_assert(kPageSize <= kArchiveSqlPageLimit);

ArchiveSqlValue SqlNumber(std::optional<qint64> value) {
	return value ? ArchiveSqlValue(int64(*value)) : ArchiveSqlValue(nullptr);
}

std::optional<qint64> ReadOptionalNumber(const ArchiveSqlValue &value) {
	return std::holds_alternative<int64>(value)
		? std::optional<qint64>(std::get<int64>(value))
		: std::nullopt;
}

QString PeerRosterJson(const std::vector<quint64> &peers) {
	auto roster = QJsonArray();
	for (const auto peer : peers) {
		roster.append(QString::number(peer));
	}
	return QString::fromUtf8(
		QJsonDocument(std::move(roster)).toJson(QJsonDocument::Compact));
}

QString FormatSqlError(const char *op, const DesktopArchiveSqlResult &result) {
	return u"%1 failed (%2, sqlite %3)"_q
		.arg(QLatin1String(op))
		.arg(result.failure.isEmpty() ? u"unknown"_q : result.failure)
		.arg(result.code);
}

ArchiveSqlStatement UpsertMusicOccurrence(
		const MusicIndexEntry &row,
		std::optional<qint64> folder = std::nullopt) {
	return {
		u"INSERT INTO music_occurrence "
		"SELECT ?1,?2,?3,?4,?5,?6,?7,?8 WHERE ?9 IS NULL OR ("
		"(SELECT filter FROM music_folder WHERE id = 1) = ?9 "
		"AND EXISTS (SELECT 1 FROM music_source WHERE peer = ?1)) "
		"ON CONFLICT(peer, message) DO UPDATE SET document=excluded.document, "
		"posted_at=excluded.posted_at, title=excluded.title, performer=excluded.performer, "
		"duration=excluded.duration, search_text=excluded.search_text"_q,
		{ QString::number(row.peer), int64(row.message),
			QString::number(row.document), int64(row.postedAt), row.title,
			row.performer, int64(row.duration), row.searchText.toCaseFolded(),
			SqlNumber(folder) },
		ArchiveSqlExpectation::OneChange,
	};
}

} // namespace

MusicIndex::MusicIndex(std::function<bool()> current)
: _current(std::move(current)) {
}

bool MusicIndex::open(const QString &path) {
	_lastError.clear();
	const auto opened = _database.Open(path, true);
	if (!opened.ok) {
		_lastError = FormatSqlError("sqlite open", opened);
		return false;
	}
	const auto configured = _database.Configure();
	if (!configured.ok) {
		_lastError = FormatSqlError("sqlite configure", configured);
		return false;
	}
	return write({
		{ u"CREATE TABLE IF NOT EXISTS music_folder(id INTEGER PRIMARY KEY CHECK(id = 1), "
		  "filter INTEGER NOT NULL, generation INTEGER NOT NULL CHECK(generation >= 0))"_q },
		{ u"INSERT OR IGNORE INTO music_folder VALUES(1, 0, 0)"_q,
			{}, ArchiveSqlExpectation::AtMostOneChange },
		{ u"CREATE TABLE IF NOT EXISTS music_source(peer TEXT PRIMARY KEY, "
		  "older_before INTEGER CHECK(older_before > 0), newest_seen INTEGER CHECK(newest_seen > 0), "
		  "complete INTEGER NOT NULL DEFAULT 0 CHECK(complete IN(0, 1))) WITHOUT ROWID"_q },
		{ u"CREATE TABLE IF NOT EXISTS music_head("
		  "peer TEXT PRIMARY KEY REFERENCES music_source(peer) ON DELETE CASCADE, "
		  "older_before INTEGER NOT NULL CHECK(older_before > 0), "
		  "newest_seen INTEGER NOT NULL CHECK(newest_seen >= older_before)) WITHOUT ROWID"_q },
		{ u"CREATE TABLE IF NOT EXISTS music_occurrence("
		  "peer TEXT NOT NULL REFERENCES music_source(peer) ON DELETE CASCADE, "
		  "message INTEGER NOT NULL CHECK(message BETWEEN 1 AND 2147483647), "
		  "document TEXT NOT NULL, posted_at INTEGER NOT NULL CHECK(posted_at > 0), "
		  "title TEXT NOT NULL, performer TEXT NOT NULL, duration INTEGER NOT NULL CHECK(duration >= 0), "
		  "search_text TEXT NOT NULL, PRIMARY KEY(peer, message)) WITHOUT ROWID"_q },
		{ u"CREATE INDEX IF NOT EXISTS music_by_document ON music_occurrence("
		  "document, posted_at DESC, peer, message DESC)"_q },
		{ u"CREATE INDEX IF NOT EXISTS music_by_recent ON music_occurrence("
		  "posted_at DESC, document, peer, message DESC)"_q },
	});
}

std::optional<MusicIndexScope> MusicIndex::configureSources(
		qint64 folder,
		const std::vector<quint64> &peers) {
	_lastError.clear();
	const auto json = PeerRosterJson(peers);
	const auto configured = write({
		{ u"UPDATE music_folder SET generation = generation + 1 "
			"WHERE id = 1 AND (filter != ?1 "
			"OR EXISTS (SELECT peer FROM music_source EXCEPT SELECT value FROM json_each(?2)) "
			"OR EXISTS (SELECT value FROM json_each(?2) EXCEPT SELECT peer FROM music_source))"_q,
			{ int64(folder), json }, ArchiveSqlExpectation::AtMostOneChange },
		{ u"DELETE FROM music_source WHERE (SELECT filter FROM music_folder) != ?"_q,
			{ int64(folder) }, ArchiveSqlExpectation::AnyChanges },
		{ u"UPDATE music_folder SET filter = ? WHERE id = 1"_q,
			{ int64(folder) }, ArchiveSqlExpectation::OneChange },
		{ u"INSERT OR IGNORE INTO music_source(peer) SELECT value FROM json_each(?)"_q,
			{ json }, ArchiveSqlExpectation::AnyChanges },
		{ u"DELETE FROM music_source WHERE peer NOT IN (SELECT value FROM json_each(?))"_q,
			{ json }, ArchiveSqlExpectation::AnyChanges },
	});
	if (!configured) return std::nullopt;
	const auto state = _database.Read({
		u"SELECT generation FROM music_folder WHERE id = 1"_q,
		{}, ArchiveSqlExpectation::Rows,
	});
	if (!state.ok || state.rows.empty()) {
		_lastError = state.ok
			? u"music_folder state row missing"_q
			: FormatSqlError("sqlite read generation", state);
		return std::nullopt;
	}
	return MusicIndexScope{ std::get<int64>(state.rows.front()[0]) };
}

std::optional<std::vector<MusicSourceCursor>> MusicIndex::sources() {
	_lastError.clear();
	auto after = QString();
	auto sources = std::vector<MusicSourceCursor>();
	while (true) {
		const auto result = _database.Read({
			u"SELECT s.peer, s.older_before, s.newest_seen, s.complete, "
			"h.older_before, h.newest_seen FROM music_source s "
			"LEFT JOIN music_head h ON h.peer = s.peer "
			"WHERE s.peer > ? ORDER BY s.peer LIMIT ?"_q,
			{ after, kPageSize }, ArchiveSqlExpectation::Rows,
		});
		if (!result.ok) {
			_lastError = FormatSqlError("sqlite read sources", result);
			return std::nullopt;
		}
		for (const auto &row : result.rows) {
			sources.push_back({
				.peer = std::get<QString>(row[0]).toULongLong(),
				.before = ReadOptionalNumber(row[1]),
				.latest = ReadOptionalNumber(row[2]),
				.complete = std::get<int64>(row[3]) == 1,
				.headBefore = ReadOptionalNumber(row[4]),
				.headNewest = ReadOptionalNumber(row[5]),
			});
		}
		if (result.rows.size() < kPageSize) return sources;
		after = std::get<QString>(result.rows.back()[0]);
	}
}

bool MusicIndex::savePage(
		MusicIndexScope scope,
		const MusicSourceCursor &expected,
		const MusicSourceCursor &cursor,
		const std::vector<MusicIndexEntry> &rows) {
	_lastError.clear();
	if (rows.size() > kPageSize || !cursor.peer || expected.peer != cursor.peer
		|| cursor.headBefore.has_value() != cursor.headNewest.has_value()) {
		_lastError = u"invalid page arguments"_q;
		return false;
	}
	const auto peer = QString::number(cursor.peer);
	auto statements = std::vector<ArchiveSqlStatement>();
	for (const auto &row : rows) {
		if (row.peer != cursor.peer || !row.document) {
			_lastError = u"invalid track entry"_q;
			return false;
		}
		statements.push_back(UpsertMusicOccurrence(row));
	}
	statements.emplace_back(
		u"UPDATE music_source SET older_before = ?, newest_seen = ?, complete = ? WHERE peer = ? "
		"AND (SELECT generation FROM music_folder WHERE id = 1) = ? "
		"AND older_before IS ? AND newest_seen IS ? AND complete = ? "
		"AND (SELECT older_before FROM music_head WHERE peer = music_source.peer) IS ? "
		"AND (SELECT newest_seen FROM music_head WHERE peer = music_source.peer) IS ?"_q,
		std::vector<ArchiveSqlValue>{ SqlNumber(cursor.before), SqlNumber(cursor.latest),
			int64(cursor.complete), peer, int64(scope.generation),
			SqlNumber(expected.before), SqlNumber(expected.latest), int64(expected.complete),
			SqlNumber(expected.headBefore), SqlNumber(expected.headNewest) },
		ArchiveSqlExpectation::OneChange);
	if (cursor.headBefore) {
		statements.push_back({
			u"INSERT INTO music_head VALUES(?, ?, ?) ON CONFLICT(peer) "
			"DO UPDATE SET older_before=excluded.older_before, newest_seen=excluded.newest_seen"_q,
			{ peer, SqlNumber(cursor.headBefore), SqlNumber(cursor.headNewest) },
			ArchiveSqlExpectation::OneChange,
		});
	} else if (expected.headBefore) {
		statements.push_back({
			u"DELETE FROM music_head WHERE peer = ?"_q,
			{ peer },
			ArchiveSqlExpectation::OneChange,
		});
	}
	return write(std::move(statements));
}

bool MusicIndex::saveArrival(qint64 folder, const MusicIndexEntry &row) {
	_lastError.clear();
	return folder > 0 && row.peer && row.document
		&& write({ UpsertMusicOccurrence(row, folder) });
}

std::optional<std::vector<MusicIndexEntry>> MusicIndex::page(
		MusicIndexScope scope,
		const QString &query,
		const std::vector<quint64> &visiblePeers,
		std::optional<MusicIndexCursor> cursor,
		bool tail) {
	_lastError.clear();
	const auto result = _database.Read({
		u"WITH visible(peer) AS (SELECT value FROM json_each(?4)), "
		"terms(term) AS (SELECT value FROM json_each(?3)) "
		"SELECT t.peer, t.message, t.document, t.posted_at, t.title, t.performer, t.duration "
		"FROM music_occurrence AS t "
		"WHERE (?1 IS NULL OR t.posted_at < ?1 OR (t.posted_at = ?1 AND t.document > ?2)) "
		"AND (SELECT generation FROM music_folder WHERE id = 1) = ?6 "
		"AND t.peer IN visible AND NOT EXISTS (SELECT 1 FROM terms WHERE instr(t.search_text, term) = 0) "
		"AND NOT EXISTS (SELECT 1 FROM music_occurrence AS a "
		"WHERE a.document = t.document AND a.peer IN visible "
		"AND NOT EXISTS (SELECT 1 FROM terms WHERE instr(a.search_text, term) = 0) "
		"AND (a.posted_at > t.posted_at OR (a.posted_at = t.posted_at "
		"AND (a.peer < t.peer OR (a.peer = t.peer AND a.message > t.message))))) "
		"ORDER BY "_q + (tail ? u"t.posted_at, t.document DESC"_q
			: u"t.posted_at DESC, t.document"_q) + u" LIMIT ?5"_q,
		{ cursor ? ArchiveSqlValue(int64(cursor->postedAt)) : ArchiveSqlValue(nullptr),
			cursor ? QString::number(cursor->document) : QString(),
			QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(
				ExactSearchTerms(query))).toJson(QJsonDocument::Compact)),
			PeerRosterJson(visiblePeers), kPageSize,
			int64(scope.generation) },
		ArchiveSqlExpectation::Rows,
	});
	if (!result.ok) {
		_lastError = FormatSqlError("sqlite read page", result);
		return std::nullopt;
	}
	auto rows = std::vector<MusicIndexEntry>();
	for (const auto &row : result.rows) {
		rows.push_back({
			.peer = std::get<QString>(row[0]).toULongLong(),
			.message = std::get<int64>(row[1]),
			.document = std::get<QString>(row[2]).toULongLong(),
			.postedAt = std::get<int64>(row[3]),
			.title = std::get<QString>(row[4]),
			.performer = std::get<QString>(row[5]),
			.duration = std::get<int64>(row[6]),
		});
	}
	if (tail) std::ranges::reverse(rows);
	return rows;
}

std::optional<qint64> MusicIndex::count(
		MusicIndexScope scope,
		const std::vector<quint64> &visiblePeers) {
	_lastError.clear();
	const auto result = _database.Read({
		u"WITH visible(peer) AS (SELECT value FROM json_each(?1)) "
		"SELECT COUNT(DISTINCT document) FROM music_occurrence "
		"WHERE (SELECT generation FROM music_folder WHERE id = 1) = ?2 "
		"AND peer IN visible"_q,
		{ PeerRosterJson(visiblePeers), int64(scope.generation) },
		ArchiveSqlExpectation::OneRow,
	});
	if (!result.ok || result.rows.empty() || result.rows.front().empty()) {
		_lastError = result.ok
			? u"sqlite count returned no row"_q
			: FormatSqlError("sqlite count", result);
		return std::nullopt;
	}
	return std::get<int64>(result.rows.front()[0]);
}

bool MusicIndex::write(std::vector<ArchiveSqlStatement> statements) {
	if (_current && !_current()) {
		_lastError = u"index authority revoked"_q;
		return false;
	}
	statements.insert(statements.begin(), { u"BEGIN IMMEDIATE"_q });
	statements.emplace_back(u"COMMIT"_q);
	const auto result = _database.Execute({
		.status = ArchiveSqlStatus::Ready,
		.statements = std::move(statements),
	}, _current);
	if (!result.committed) {
		_lastError = FormatSqlError("sqlite write", result);
	}
	return result.committed;
}

} // namespace Purple

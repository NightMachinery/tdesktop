#pragma once

#include "purple/purple_archive_sqlite.h"

namespace Purple {

struct MusicIndexEntry {
	quint64 peer = 0;
	qint64 message = 0;
	quint64 document = 0;
	qint64 postedAt = 0;
	QString title;
	QString performer;
	qint64 duration = 0;
	QString searchText;
};

struct MusicIndexCursor {
	qint64 postedAt = 0;
	quint64 document = 0;
};

struct MusicSourceCursor {
	quint64 peer = 0;
	std::optional<qint64> before;
	std::optional<qint64> latest;
	bool complete = false;
};

struct MusicIndexScope {
	qint64 generation = 0;
};

class MusicIndex final {
public:
	explicit MusicIndex(std::function<bool()> current = {});
	[[nodiscard]] bool open(const QString &path);
	[[nodiscard]] std::optional<MusicIndexScope> configureSources(
		qint64 folder,
		const std::vector<quint64> &peers);
	[[nodiscard]] std::optional<std::vector<MusicSourceCursor>> sources();
	[[nodiscard]] bool savePage(
		MusicIndexScope scope,
		const MusicSourceCursor &expected,
		const MusicSourceCursor &cursor,
		const std::vector<MusicIndexEntry> &rows);
	[[nodiscard]] bool saveArrival(qint64 folder, const MusicIndexEntry &row);
	[[nodiscard]] std::optional<std::vector<MusicIndexEntry>> page(
		MusicIndexScope scope,
		const QString &query,
		const std::vector<quint64> &visiblePeers,
		std::optional<MusicIndexCursor> cursor);

private:
	[[nodiscard]] bool write(std::vector<ArchiveSqlStatement> statements);
	DesktopArchiveSqlite _database;
	std::function<bool()> _current;

};

} // namespace Purple

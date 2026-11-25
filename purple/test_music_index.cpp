#include "purple/purple_music_index.cpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>
#include <cstdlib>
#include <iostream>

namespace {
void Check(bool passed, const char *label) {
	if (!passed) {
		std::cerr << label << '\n';
		std::exit(1);
	}
}

Purple::MusicIndexEntry Track(quint64 peer, qint64 msg, quint64 doc, qint64 stamp) {
	return { peer, msg, doc, stamp, u"Glass Orchard"_q, u"Demo Artist"_q,
		125, u"Glass Orchard Demo Artist"_q };
}
} // namespace

int main(int argc, char **argv) {
	const auto app = QCoreApplication(argc, argv);
	auto temporary = QTemporaryDir();
	Check(temporary.isValid(), "private fixture");
	const auto dir = QFileInfo(temporary.path()).canonicalFilePath();
	Check(!dir.isEmpty(), "canonical fixture dir");
	const auto path = dir + u"/music.sqlite"_q;
	const auto maximum = std::numeric_limits<quint64>::max();
	const auto arrivalsPath = dir + u"/arrivals.sqlite"_q;
	auto arrivalScope = Purple::MusicIndexScope();
	{
		auto index = Purple::MusicIndex();
		Check(index.open(arrivalsPath), ("open arrival fixture: " + index.lastError().toStdString()).c_str());
		Check(!index.saveArrival(42, Track(7, 90, 90, 110)), "unconfigured arrival rejected");
		const auto scope = index.configureSources(42, {7});
		Check(bool(scope), "configure arrival folder");
		arrivalScope = *scope;
		Check(index.savePage(*scope, {7}, {7, 20, 22, false},
			{Track(7, 21, 21, 100)}), "seed history cursor");
		Check(index.saveArrival(42, Track(7, 90, 90, 120)), "record new track independently");
		Check(index.saveArrival(42, Track(7, 90, 90, 120)), "duplicate delivery is idempotent");
		Check(!index.saveArrival(43, Track(7, 91, 91, 121))
			&& !index.saveArrival(42, Track(8, 92, 92, 122)),
			"arrival requires both applied folder and source membership");
		const auto sources = index.sources();
		Check(sources && sources->size() == 1 && sources->front().latest == 22
			&& sources->front().before == 20 && !sources->front().complete,
			"arrival advances neither history checkpoint");
		auto head = sources->front();
		head.headBefore = 50;
		head.headNewest = 90;
		Check(index.savePage(*scope, sources->front(), head,
			{Track(7, 50, 50, 110)}), "checkpoint newer-track reconciliation");
	}
	{
		auto index = Purple::MusicIndex();
		Check(index.open(arrivalsPath), "reopen arrival fixture");
		const auto sources = index.sources();
		Check(sources && sources->size() == 1 && sources->front().latest == 22
			&& sources->front().headBefore == 50 && sources->front().headNewest == 90,
			"resumed head scan remembers the page and the previous watermark");
		Check(index.savePage(arrivalScope, sources->front(), {7, 20, 90, false},
			{Track(7, 90, 90, 120)}), "finishing the head scan promotes the watermark");
		const auto finished = index.sources();
		Check(finished && !finished->front().headBefore && !finished->front().headNewest,
			"finishing the head scan clears its checkpoint");
		const auto list = index.page(arrivalScope, u""_q, {7, 8}, {});
		Check(list && list->size() == 3 && list->front().document == 90,
			"arrivals persist once and sort by posted date");
		Check(bool(index.configureSources(43, {7})), "replace the source folder");
		Check(!index.saveArrival(42, Track(7, 93, 93, 130)), "retired folder arrival rejected");
	}
	auto initial = Purple::MusicIndexScope();
	{
		auto index = Purple::MusicIndex();
		Check(index.open(path), "open and initialize database schema");
		const auto scope = index.configureSources(42, {7, 8});
		Check(bool(scope), "select initial folder and source roster atomically");
		initial = *scope;
		Check(index.savePage(initial, {7}, {7, 20, 22, false}, {Track(7, 21, maximum, 100)})
			&& index.savePage(initial, {8}, {8, 11, 13, false},
				{Track(8, 12, maximum, 110), Track(8, 13, 17, 110)}), "store two pages");
		const auto list = index.page(initial, u"gLASS orCHard"_q, {7, 8}, {});
		Check(list && list->size() == 2 && list->back().peer == 8
			&& list->back().document == maximum, "unsigned identity and deduplication");
		const auto exact = index.page(initial, u"artist  ORCh   glass"_q, {7, 8}, {});
		Check(exact && exact->size() == 2 && exact->back().document == maximum, "exact substrings in any order match the same songs");
		const auto gaps = index.page(initial, u"gass ORCh"_q, {7, 8}, {});
		Check(gaps && gaps->empty(), "letters separated by gaps are not an exact match");
		const auto literal = index.page(initial, u"glass %"_q, {7, 8}, {});
		Check(literal && literal->empty(), "wildcards in the query remain literal");
		const auto next = index.page(initial, u""_q, {7, 8}, Purple::MusicIndexCursor{110, 17});
		Check(next && next->size() == 1 && next->front().document == maximum, "keyset continuation");
		const auto visible = index.page(initial, u"glass"_q, {7}, {});
		Check(visible && visible->size() == 1 && visible->front().peer == 7
			&& visible->front().document == maximum, "hidden recent alias cannot hide visible track");
		const auto hidden = index.page(initial, u""_q, {}, {});
		Check(hidden && hidden->empty(), "no visible source returns no rows");
		const auto totalBoth = index.count(initial, {7, 8});
		Check(totalBoth && *totalBoth == 2, "count deduplicates distinct visible tracks");
		const auto totalOne = index.count(initial, {7});
		Check(totalOne && *totalOne == 1, "count respects visible peer subset");
		const auto totalHidden = index.count(initial, {});
		Check(totalHidden && *totalHidden == 0, "count with no visible peers is zero");
		Check(!index.savePage(initial, {8, 11, 13, false}, {8, 10, 13, false}, {Track(8, 11, 22, 120), Track(8, 0, 23, 121)}),
			"invalid page rejected");
		Check(!index.lastError().isEmpty(), "failed write records diagnostic error");

		Check(!index.savePage(initial, {8}, {8, 10, 14, true}, {Track(8, 14, 24, 122)}),
			"older scanner cannot update a newer cursor or add its rows");
		const auto intact = index.sources();
		Check(intact && intact->size() == 2 && intact->back().before == 11,
			"cursor rolled back with failed page");
		const auto after = index.page(initial, u""_q, {7, 8}, {});
		Check(after && after->size() == 2, "rows rolled back with failed page");
	}
	{
		auto index = Purple::MusicIndex();
		Check(index.open(path), "reopen");
		const auto same = index.configureSources(42, {8, 7, 8});
		Check(same && same->generation == initial.generation, "identical roster preserves the scope");
		const auto live = index.page(*same, u"demo artist"_q, {7, 8}, {});
		Check(live && live->size() == 2, "rows persist");
		const auto reduced = index.configureSources(42, {7});
		Check(reduced && reduced->generation != initial.generation, "remove a source and advance scope");

		const auto stale = index.page(initial, u""_q, {7}, {});
		Check(stale && stale->empty(), "retired reader gets no entries even when its peer remains");
		const auto alias = index.page(*reduced, u""_q, {7, 8}, {});
		Check(alias && alias->size() == 1 && alias->front().peer == 7
			&& alias->front().document == maximum, "remaining alias wins");
		Check(bool(index.configureSources(67, {7})), "choose a different source folder");
		const auto empty = index.page(*reduced, u""_q, {7, 8}, {});
		Check(empty && empty->empty(), "old scope removed");
		const auto reselected = index.configureSources(42, {7});
		Check(bool(reselected), "reselect original folder");
		Check(!index.savePage(*reduced, {7}, {7, 20, 22, false}, {Track(7, 21, maximum, 100)}),
			"late page cannot commit after a folder switch and reselect");
		const auto afterLate = index.page(*reselected, u""_q, {7}, {});
		Check(afterLate && afterLate->empty(), "late scope rolls back rows and cursor");
		auto roster = std::vector<quint64>();
		for (auto peer = quint64(1); peer <= 2500; ++peer) {
			roster.push_back(peer);
		}
		roster.push_back(maximum);
		const auto scope = index.configureSources(67, roster);
		Check(bool(scope), "replace roster in one transaction");
		const auto sources = index.sources();
		Check(sources && sources->size() == roster.size(), "page through source roster");
		auto previous = Purple::MusicSourceCursor{ maximum };
		for (auto chunk = 0; chunk != 3; ++chunk) {
			auto rows = std::vector<Purple::MusicIndexEntry>();
			for (auto row = 1; row <= 100; ++row) {
				const auto number = 100 * chunk + row;
				rows.push_back(Track(maximum, number, number, 1000 + number));
			}
			Check(index.savePage(*scope, previous, {maximum, {}, 100 * (chunk + 1), true}, rows),
				"save a large catalog");
			previous = { maximum, {}, 100 * (chunk + 1), true };
		}
		auto cursor = std::optional<Purple::MusicIndexCursor>();
		const auto oldest = index.page(*scope, u"glass"_q, roster, {}, true);
		Check(oldest && oldest->size() == 100
			&& oldest->front().message == 100 && oldest->back().message == 1,
			"filtered tail returns the oldest page in display order");
		for (auto chunk = 0; chunk != 3; ++chunk) {
			const auto rows = index.page(*scope, u""_q, roster, cursor);
			Check(rows && rows->size() == 100, "catalog page fits SQLite limit");
			Check(rows->front().message == 300 - 100 * chunk
				&& rows->back().message == 201 - 100 * chunk, "no skipped catalog rows");
			cursor = { rows->back().postedAt, rows->back().document };
		}
		Check(bool(index.configureSources(67, {maximum})), "prune a large roster");
		const auto survivor = index.sources();
		Check(survivor && survivor->size() == 1 && survivor->front().latest == 300,
			"retained source preserves its cursor");
	}

	{
		auto remaining = 100;
		auto index = Purple::MusicIndex([&] { return --remaining > 0; });
		Check(index.open(dir + u"/authority.sqlite"_q), "open with authority");
		const auto scope = index.configureSources(42, {7});
		Check(bool(scope), "source with authority");
		remaining = 2;
		Check(!index.savePage(*scope, {7}, {7, 20, 22, false}, {Track(7, 21, maximum, 100)}),
			"commit checks revocation after staging the page");
		const auto rows = index.page(*scope, u""_q, {7}, {});
		const auto cursors = index.sources();
		Check(rows && rows->empty() && cursors && cursors->size() == 1
			&& !cursors->front().before && !cursors->front().latest, "revoked write rolls back");
	}
	std::cout << "Music index persistence, pagination, visibility and transaction checks passed\n";
}

#include "purple/purple_emoji_export_path.h"
#include "purple/purple_emoji_export_worker.h"
#include "purple/purple_emoji_large_store.h"
#include "base/basic_types.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <atomic>
#include <cstdio>

int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv);
	using namespace Purple;
	auto temporary = QTemporaryDir();
	if (!temporary.isValid()) return 2;
	const auto root = QFileInfo(temporary.path()).canonicalFilePath();
	const auto directory = root + u"/emoji-export"_q;
	auto checks = std::atomic<int>(0);
	auto failures = std::atomic<int>(0);
	const auto check = [&](bool value, const char *name) {
		++checks;
		if (!value) { ++failures; std::printf("FAIL %s\n", name); }
	};
	const auto write = [](const QString &path, const QByteArray &bytes = {}) {
		auto file = QFile(path);
		return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
	};
	const auto wait = [&](auto predicate) {
		auto elapsed = QElapsedTimer();
		elapsed.start();
		while (!predicate() && elapsed.elapsed() < 30000) { app.processEvents(); QThread::msleep(1); }
		return predicate();
	};
	auto done = false;
	{
		auto worker = EmojiExportWorker(directory);
		check(worker.submit(worker.generation(), [&](const EmojiExportAuthority &current) {
			if (!EnsureEmojiLargeDirectories(directory)) return false;
			for (auto i = 1; i <= 6000; ++i) {
				if (!write(directory + '/' + EmojiLargePngName(i, 512))) return false;
			}
			auto pages = 0;
			do {
				check(PruneEmojiLargeExport(directory, 1024, current), "prune page succeeds");
				const auto stats = EmojiLargePruneStats();
				check(stats.examined <= kEmojiExportMaintenancePage
					&& stats.removed <= kEmojiExportMaintenancePage
					&& stats.retained <= kEmojiLargeMaximumFiles, "prune work and index are bounded with zero-byte files");
				++pages;
			} while (EmojiLargePruneStats().pending && pages < 100);
			check(pages > 1 && pages < 100, "prune makes finite paged progress");
			check(QDir(directory + u"/large"_q).entryList(QDir::Files).size() == kEmojiLargeMaximumFiles,
				"zero-byte large files obey independent entry cap");
			check(PruneEmojiLargeExport(directory, 0, current) && EmojiLargePruneStats().pending,
				"partial pruning index exists before revocation");
			auto retiredCleanup = EmojiExportCleanup();
			auto retiredResult = EmojiExportCleanupResult::Pending;
			while (retiredResult == EmojiExportCleanupResult::Pending) retiredResult = retiredCleanup.step(directory, current);
			check(retiredResult == EmojiExportCleanupResult::Done, "revocation retires a partial pruning index");
			check(write(directory + '/' + EmojiLargePngName(99999, 100), "fresh")
				&& PruneEmojiLargeExport(directory, 0, current)
				&& !EmojiLargePruneStats().pending
				&& !QFileInfo::exists(directory + '/' + EmojiLargePngName(99999, 100)),
				"new prune after revocation cannot reuse removed old records");
			for (auto i = 0; i < 6000; ++i) {
				if (!write(directory + u"/requests/malformed-"_q + QString::number(i))) return false;
			}
			if (!write(directory + u"/requests/77"_q)
				|| !write(directory + u"/requests/preserve"_q, "payload")) return false;
			auto scanner = EmojiLargeRequestScanner();
			auto admitted = false;
			auto overflowSeen = false;
			for (auto i = 0; i < 150; ++i) {
				const auto ids = scanner.scan(directory, current);
				const auto stats = scanner.stats();
				check(stats.examined <= kEmojiExportMaintenancePage
					&& stats.removed <= kEmojiExportMaintenancePage
					&& stats.retained <= kEmojiExportMaintenancePage, "request counting and overflow metadata are bounded");
				if (scanner.overflow()) {
					overflowSeen = true;
					check(ids.empty(), "overflow suppresses request admission");
				}
				if (std::find(ids.begin(), ids.end(), 77) != ids.end()) { admitted = true; break; }
			}
			check(overflowSeen && admitted, "malformed overflow drains before requests resume");
			check(QFileInfo::exists(directory + u"/requests/preserve"_q), "nonempty foreign files survive overflow");
			for (auto i = 10000; i < 11030; ++i) {
				if (!write(directory + u"/requests/"_q + QString::number(i))) return false;
			}
			scanner.reset();
			for (auto page = 0; page < 24; ++page) {
				check(scanner.scan(directory, current).empty(), "canonical-only overflow is never silently discarded or admitted");
			}
			check(scanner.overflow(), "canonical requests hold overflow until producer drains");
			for (auto i = 10000; i < 10064; ++i) {
				if (!QFile::remove(directory + u"/requests/"_q + QString::number(i))) return false;
			}
			admitted = false;
			for (auto page = 0; page < 30 && !admitted; ++page) admitted = !scanner.scan(directory, current).empty();
			check(admitted && !scanner.overflow(), "producer drain releases canonical-only overflow");
			if (!QDir().mkpath(directory + u"/png"_q)) return false;
			for (auto i = 1; i <= 4000; ++i) {
				if (!write(directory + u"/png/"_q + QString::number(i) + u"-128.png"_q)
					|| !write(directory + u"/requests/"_q + QString::number(i))) return false;
			}
			check(PublishEmojiExportCatalog(directory, {}, QDateTime::currentDateTimeUtc(), current)
				&& QDir(directory + u"/png"_q).entryList(QDir::Files).size() == 4000 - kEmojiExportMaintenancePage,
				"stale preview maintenance removes one bounded page from thousands of files");
			check(PublishEmojiExportCatalog(directory, {}, QDateTime::currentDateTimeUtc(), current)
				&& QDir(directory + u"/png"_q).entryList(QDir::Files).size() == 4000 - 2 * kEmojiExportMaintenancePage,
				"later publication resumes the stale preview cursor");
			auto cleanup = EmojiExportCleanup();
			auto result = EmojiExportCleanupResult::Pending;
			pages = 0;
			while (result == EmojiExportCleanupResult::Pending && pages < 100) {
				result = cleanup.step(directory, current);
				const auto stats = cleanup.stats();
				check(stats.examined <= kEmojiExportMaintenancePage
					&& stats.removed <= kEmojiExportMaintenancePage && !stats.retained,
					"revocation slice has constant retained state and bounded enumeration/removal");
				++pages;
			}
			check(result == EmojiExportCleanupResult::Done && pages > 1 && pages < 100,
				"cleanup completes across many bounded pages");
			check(!EmojiExportCleanupRecorded(directory), "completed cleanup retires persistent obligation");
			auto image = QImage(100, 100, QImage::Format_ARGB32_Premultiplied);
			image.fill(Qt::red);
			for (const auto &folder : { u"large"_q, u"png"_q }) {
				const auto path = directory + '/' + folder;
				const auto moved = directory + '/' + folder + u"-saved"_q;
				auto callbacks = 0;
				const auto authority = [&] {
					if (++callbacks == 2) {
						check(QDir().rename(path, moved) && QDir().mkdir(path), "replace parent with a different non-link directory");
					}
					return current();
				};
				const auto ok = (folder == u"large"_q)
					? WriteEmojiLargePng(directory, 9000, image, {}, authority)
					: WriteEmojiExportPng(directory, 9000, EmojiExportFrame(image, {}), authority);
				check(!ok && QDir(path).entryList(QDir::Files).isEmpty(), "parent identity replacement skips PNG publication");
				check(QDir().rmdir(path) && QDir().rename(moved, path), "restore synthetic parent");
			}
			check(WriteEmojiExportPng(directory, 9000, EmojiExportFrame(image, {}), current), "catalog replacement preview");
			auto catalogCallbacks = 0;
			const auto savedExport = root + u"/emoji-export-saved"_q;
			check(!PublishEmojiExportCatalog(directory, {{ .id = 9000 }}, QDateTime::currentDateTimeUtc(), [&] {
				if (++catalogCallbacks == 3) {
					check(QDir().rename(directory, savedExport) && QDir().mkdir(directory)
						&& write(directory + u"/catalog.json"_q, "sentinel"), "replace catalog parent before atomic rename");
				}
				return current();
			}), "catalog publication skips changed parent");
			auto sentinel = QFile(directory + u"/catalog.json"_q);
			check(sentinel.open(QIODevice::ReadOnly) && sentinel.readAll() == "sentinel", "replacement catalog sentinel survives");
			sentinel.close();
			check(QFile::remove(directory + u"/catalog.json"_q) && QDir().rmdir(directory)
				&& QDir().rename(savedExport, directory), "restore synthetic export root");
			const auto requests = directory + u"/requests"_q;
			const auto saved = directory + u"/requests-saved"_q;
			check(write(requests + u"/77"_q), "ack replacement fixture");
			auto callbacks = 0;
			check(!AcknowledgeEmojiLargeRequest(directory, 77, [&] {
				if (++callbacks == 2) {
					check(QDir().rename(requests, saved) && QDir().mkdir(requests)
						&& write(requests + u"/77"_q), "replace request parent before remove");
				}
				return current();
			}), "acknowledgement skips changed parent");
			check(QFileInfo::exists(requests + u"/77"_q) && QFileInfo::exists(saved + u"/77"_q),
				"replacement and original requests both survive skipped removal");
			check(write(directory + '/' + EmojiLargePngName(9001, 100), "owned"), "prune replacement fixture");
			const auto large = directory + u"/large"_q;
			const auto largeSaved = directory + u"/large-saved"_q;
			callbacks = 0;
			check(!PruneEmojiLargeExport(directory, 0, [&] {
				if (++callbacks == 2) {
					check(QDir().rename(large, largeSaved) && QDir().mkdir(large)
						&& write(large + u"/9001-100.png"_q, "outside"), "replace prune parent before removal");
				}
				return current();
			}), "prune skips changed parent");
			check(QFileInfo::exists(large + u"/9001-100.png"_q), "replacement prune sentinel survives");
			return current();
		}, [&](quint64, bool ok) { check(ok, "bounded store fixture completes"); done = true; }), "bounded store job accepted");
		check(wait([&] { return done; }), "bounded store job settles");
	}
	const auto stopDirectory = root + u"/quit/emoji-export"_q;
	auto entered = std::atomic<bool>(false);
	auto worker = std::make_unique<EmojiExportWorker>(stopDirectory);
	check(worker->submit(worker->generation(), [&](const EmojiExportAuthority &current) {
		if (!EnsureEmojiLargeDirectories(stopDirectory)) return false;
		for (auto i = 1; i <= 5000; ++i) {
			if (!write(stopDirectory + u"/requests/"_q + QString::number(i))) return false;
		}
		entered.store(true);
		while (current()) QThread::msleep(1);
		QThread::msleep(30);
		return false;
	}, {}), "quit fixture blocks active worker before queued cleanup");
	check(wait([&] { return entered.load(); }), "quit fixture ready");
	worker->revoke(true, {});
	auto elapsed = QElapsedTimer();
	elapsed.start();
	worker.reset();
	check(elapsed.elapsed() < 1000, "shutdown joins without traversing pending directory");
	check(EmojiExportCleanupRecorded(stopDirectory)
		&& QDir(stopDirectory + u"/requests"_q).entryList(QDir::Files).size() == 5000,
		"shutdown records obligation and leaves all pending entries for next start");
	done = false;
	{
		auto restarted = EmojiExportWorker(stopDirectory);
		check(restarted.submit(restarted.generation(), [&](const EmojiExportAuthority &) {
			return QDir(stopDirectory + u"/requests"_q).entryList(QDir::Files).isEmpty()
				&& !EmojiExportCleanupRecorded(stopDirectory);
		}, [&](quint64, bool ok) { check(ok, "restart finishes recorded cleanup before new work"); done = true; }),
			"restart job admitted behind persisted barrier");
		check(wait([&] { return done; }), "restart barrier settles");
	}
	std::printf("emoji large bounds: %d checks, %d failures\n", checks.load(), failures.load());
	return failures.load() ? 1 : 0;
}

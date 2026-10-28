#include "purple/purple_emoji_large_store.h"
#include "purple/purple_emoji_export_worker.h"
#include "base/basic_types.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <atomic>
#include <cstdio>
#include <set>
#include <thread>

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	auto app = QCoreApplication(argc, argv);
	using namespace Purple;
	auto temporary = QTemporaryDir();
	if (!temporary.isValid()) return 2;
	const auto directory = QFileInfo(temporary.path()).canonicalFilePath() + u"/emoji-export"_q;
	auto worker = EmojiExportWorker(directory);
	auto checks = std::atomic<int>(0);
	auto failures = std::atomic<int>(0);
	const auto check = [&](bool value, const char *name) {
		++checks;
		if (!value) { ++failures; std::printf("FAIL %s\n", name); }
	};
	const auto mainThread = std::this_thread::get_id();
	auto done = false;
	auto ok = false;
	auto heartbeats = 0;
	auto timer = QTimer();
	QObject::connect(&timer, &QTimer::timeout, [&] { ++heartbeats; });
	timer.start(1);
	const auto readCatalog = [&] {
		auto file = QFile(directory + u"/catalog.json"_q);
		if (!file.open(QIODevice::ReadOnly)) return QJsonObject();
		return QJsonDocument::fromJson(file.readAll()).object();
	};
	const auto work = [&](const EmojiExportAuthority &current) {
		check(std::this_thread::get_id() != mainThread, "storage runs on actual production worker");
		auto previous = std::vector<EmojiExportEntry>{
			{.id = 50, .setId = 1}, {.id = 51, .setId = 1}, {.id = 52, .setId = 2}};
		auto ready = std::vector<std::optional<EmojiExportEntry>>{
			EmojiExportEntry{.id = 51, .setId = 1, .setTitle = u"Known"_q,
				.setShortName = u"known"_q, .installed = false}, std::nullopt,
			EmojiExportEntry{.id = 53, .setId = 3}};
		const auto partial = MergeEmojiLargePublication(previous, ready, false, current);
		check(partial.size() == 4 && partial[0].id == 50 && partial[3].id == 53,
			"partial publication retains older catalog and adds demanded entry");
		check(partial.size() == 4 && partial[0].setTitle == u"Known"_q
			&& partial[1].setShortName == u"known"_q && partial[2].setTitle.isEmpty()
			&& partial[0].installed == false, "new title enriches every retained matching-set entry");
		const auto rolled = MergeEmojiLargePublication(previous, ready, false, current, {50});
		check(rolled.size() == 3 && rolled[0].id == 51 && rolled[2].id == 53,
			"retired rolling demand reference is omitted without dropping other catalog records");
		const auto finished = MergeEmojiLargePublication(previous, ready, true, current);
		check(finished.size() == 2 && finished[0].id == 51 && finished[1].id == 53,
			"complete pass replaces prior catalog in current candidate order");
		check(MergeEmojiLargePublication(previous, ready, false, [] { return false; }).empty(),
			"revoked merge cannot produce a publishable prior snapshot");
		const auto prepared = EnsureEmojiLargeDirectories(directory);
		check(prepared, "safe output and request directories");
		if (!prepared) return false;
		check(EmojiLargePngName(0, 512).isEmpty() && EmojiLargePngName(1, 513).isEmpty(), "owned filename bounds");
		auto image = QImage(512, 512, QImage::Format_ARGB32_Premultiplied);
		image.fill(QColor(255, 0, 0, 64));
		auto native = QImage(100, 60, QImage::Format_ARGB32_Premultiplied);
		native.fill(Qt::transparent);
		check(WriteEmojiLargePng(directory, 1, image, std::nullopt, current), "atomic vector-size PNG");
		check(WriteEmojiLargePng(directory, 2, native, std::nullopt, current), "native raster PNG");
		check(QImage(directory + '/' + EmojiLargePngName(2, 100)).size() == QSize(100, 60), "native size is never enlarged or padded");
		check(WriteEmojiLargePng(directory, 3, image, QColor(Qt::green), current), "template tint written");
		const auto tinted = QImage(directory + '/' + EmojiLargePngName(3, 512));
		check(tinted.hasAlphaChannel() && qAlpha(tinted.pixel(0, 0)) == 64 && qGreen(tinted.pixel(0, 0)) > 0, "tint preserves alpha");
		check(!WriteEmojiLargePng(directory, 4, QImage(513, 512, QImage::Format_ARGB32), {}, current), "oversized frame refused");
		auto fences = 0;
		check(!WriteEmojiLargePng(directory, 5, image, {}, [&] { return current() && ++fences < 3; }), "authority checked at atomic commit");
		check(!QFileInfo::exists(directory + '/' + EmojiLargePngName(5, 512)), "revoked atomic write publishes nothing");
		auto entries = std::vector<EmojiExportEntry>{
			{ .id = 1, .large = EmojiLargePngName(1, 512), .sourceKind = u"tgs"_q, .sourcePx = 512, .installed = true },
			{ .id = 2, .large = EmojiLargePngName(2, 100), .sourceKind = u"webp"_q, .sourcePx = 100, .installed = false },
		};
		for (const auto &entry : entries) check(WriteEmojiExportPng(directory, entry.id, EmojiExportFrame(image, {}), current), "ordinary preview retained");
		check(PublishEmojiExportCatalog(directory, entries, QDateTime::currentDateTimeUtc(), current), "compatible catalog publication");
		const auto catalog = readCatalog();
		const auto array = catalog.value(u"entries"_q).toArray();
		check(catalog.value(u"version"_q).toInt() == 1 && array.size() == 2, "catalog remains version one");
		if (array.size() != 2) return false;
		check(array[0].toObject().value(u"sourceKind"_q).toString() == u"tgs"_q && array[0].toObject().value(u"sourcePx"_q).toInt() == 512, "source metadata exact");
		check(array[0].toObject().value(u"installed"_q).toBool() && !array[1].toObject().value(u"installed"_q).toBool(true), "installed optional false remains explicit");
		check(!EmojiExportCatalog({EmojiExportEntry{.id=9}}, QDateTime::currentDateTimeUtc()).value(u"entries"_q).toArray()[0].toObject().contains(u"large"_q), "old entries omit new optional fields");
		for (auto id = 1; id <= 3; ++id) {
			auto file = QFile(directory + '/' + EmojiLargePngName(id, id == 2 ? 100 : 512));
			check(file.open(QIODevice::ReadOnly) && file.setFileTime(QDateTime::fromSecsSinceEpoch(1000 + id), QFileDevice::FileModificationTime), "synthetic LRU order");
		}
		check(TouchEmojiLargePng(directory, EmojiLargePngName(1, 512), current), "demand use refreshes LRU age");
		const auto keepBytes = QFileInfo(directory + '/' + EmojiLargePngName(1, 512)).size() + QFileInfo(directory + '/' + EmojiLargePngName(3, 512)).size();
		check(PruneEmojiLargeExport(directory, keepBytes, current), "lowered cap prunes immediately");
		check(QFileInfo::exists(directory + '/' + EmojiLargePngName(1, 512)) && !QFileInfo::exists(directory + '/' + EmojiLargePngName(2, 100)), "least recently used removed before recent demand");
		check(!readCatalog().value(u"entries"_q).toArray()[1].toObject().contains(u"large"_q), "catalog reference retracted before removal");
		check(PublishEmojiExportCatalog(directory, entries, QDateTime::currentDateTimeUtc(), current) && !readCatalog().value(u"entries"_q).toArray()[1].toObject().contains(u"large"_q), "old ready snapshot cannot resurrect pruned reference");
		check(QFileInfo::exists(directory + '/' + EmojiExportPngName(2)), "preview excluded from cap");
		const auto request = [&](const QString &name, const QByteArray &bytes = {}) {
			auto file = QFile(directory + u"/requests/"_q + name);
			return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
		};
		auto made = true;
		for (auto i = 0; i != 400; ++i) made = request(u"invalid-"_q + QString::number(i)) && made;
		check(made && request(u"42"_q) && request(u"45"_q) && request(u"44"_q, "nonempty") && request(u"01"_q), "request and invalid-prefix fixtures");
		const auto outside = temporary.path() + u"/outside"_q;
		{ auto file = QFile(outside); check(file.open(QIODevice::WriteOnly), "outside sentinel created"); }
		check(QFile::link(outside, directory + u"/requests/46"_q), "request symlink fixture");
		auto scanner = EmojiLargeRequestScanner();
		auto found = std::set<quint64>();
		for (auto step = 0; step != 200 && found.size() != 2; ++step) {
			const auto page = scanner.scan(directory, current, 7, 1);
			check(page.size() <= 1, "request page bounded");
			for (const auto id : page) { found.insert(id); check(AcknowledgeEmojiLargeRequest(directory, id, current), "completed request acknowledged"); }
		}
		check(found == std::set<quint64>{42,45}, "invalid prefix cannot starve later valid requests");
		check(!AcknowledgeEmojiLargeRequest(directory, 44, current) && !AcknowledgeEmojiLargeRequest(directory, 46, current), "nonempty and symlink requests untouched");
		check(QFileInfo::exists(outside) && QFileInfo::exists(directory + u"/requests/01"_q), "unrelated files preserved");
		check(PruneEmojiLargeExport(directory, 0, current), "zero test budget removes owned large files");
		check(QFileInfo::exists(directory + '/' + EmojiExportPngName(1)) && QFileInfo::exists(directory + u"/catalog.json"_q), "cap cleanup preserves preview and catalog");
		return current();
	};
	check(worker.submit(worker.generation(), work, [&](quint64, bool result) {ok=result;done=true;}), "store job accepted");
	auto elapsed = QElapsedTimer();
	elapsed.start();
	while (!done && elapsed.elapsed() < 15000) { app.processEvents(); QThread::msleep(1); }
	check(done && ok && heartbeats > 0, "main loop heartbeat during storage work");
	auto entered = std::atomic<bool>(false);
	done = false;
	check(worker.submit(worker.generation(), [&](const EmojiExportAuthority &current) {
		entered.store(true);
		while (current()) QThread::msleep(1);
		QThread::msleep(10);
		return WriteEmojiLargePng(directory, 88, QImage(100, 100, QImage::Format_ARGB32), {}, current);
	}, [](quint64, bool) {}), "revocation worker job accepted");
	elapsed.restart();
	while (!entered.load() && elapsed.elapsed() < 5000) { app.processEvents(); QThread::msleep(1); }
	const auto before = heartbeats;
	worker.revoke(true, [&](quint64, bool result) {ok=result;done=true;});
	elapsed.restart();
	while (!done && elapsed.elapsed() < 15000) { app.processEvents(); QThread::msleep(1); }
	check(done && ok && heartbeats > before, "revocation barrier completes without blocking main loop");
	check(!QFileInfo::exists(directory + '/' + EmojiLargePngName(88, 100)) && !QFileInfo::exists(directory + u"/catalog.json"_q), "revoked worker cannot recreate output");
	std::printf("emoji large store: %d checks, %d failures\n", checks.load(), failures.load());
	return failures.load() ? 1 : 0;
}

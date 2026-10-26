#include "purple/purple_emoji_export_queue.h"
#include "purple/purple_emoji_export_store.h"
#include "purple/purple_emoji_export_worker.h"
#include "base/basic_types.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <set>
#include <thread>
#ifndef Q_OS_WIN
#include <unistd.h>
#endif

using namespace Purple;

int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv);
	auto checks = 0;
	auto failures = 0;
	const auto check = [&](bool ok, const char *name) {
		++checks;
		if (!ok) { ++failures; std::printf("FAIL %s\n", name); }
	};
	auto candidates = std::vector<quint64>();
	for (auto i = quint64(1); i <= 305; ++i) candidates.push_back(i);
	auto queue = EmojiExportQueue(100, 1000);
	queue.begin(candidates, { 305 });
	check(queue.current() == 304, "extra ID reserved before ordinary resolution budget");
	auto completed = std::set<quint64>();
	auto continuations = 0;
	auto requests = 0;
	while (const auto index = queue.current()) {
		const auto id = candidates[*index];
		const auto admission = queue.resolve(id, 0);
		if (admission == EmojiResolveAdmission::Continue) {
			++continuations;
			check(requests == 100, "slice budget bounded");
			requests = 0;
			queue.continueSlice();
			check(queue.current() == index, "continuation retains deferred cursor");
			continue;
		}
		check(admission == EmojiResolveAdmission::Ready, "available metadata admitted");
		++requests;
		completed.insert(id);
		queue.succeeded(id);
		queue.next();
	}
	check(completed.size() == 305 && continuations == 3, "overflow finishes from internal continuations");
	check(completed.contains(305) && !queue.retryAt(), "extra ID completes without external event");
	queue.begin(candidates, { 305 });
	completed.clear();
	while (const auto index = queue.current()) {
		const auto id = candidates[*index];
		const auto admission = queue.resolve(id, 10);
		if (admission == EmojiResolveAdmission::Continue) { queue.continueSlice(); continue; }
		if (id <= 200) queue.failed(id, 10);
		else { completed.insert(id); queue.succeeded(id); }
		queue.next();
	}
	check(completed.size() == 105 && completed.contains(305), "failing prefix cannot starve tail or extra");
	check(queue.retryAt() == 1010, "failure arms an automatic cooldown deadline");
	queue.begin(candidates, { 305 });
	auto cooldowns = 0;
	requests = 0;
	while (const auto index = queue.current()) {
		const auto id = candidates[*index];
		const auto admission = queue.resolve(id, 20);
		if (admission == EmojiResolveAdmission::Continue) { queue.continueSlice(); continue; }
		if (admission == EmojiResolveAdmission::Cooldown) ++cooldowns;
		else ++requests;
		queue.next();
	}
	check(cooldowns == 200 && requests == 105, "refresh cannot reset failed-prefix cooldown");
	queue.begin(candidates, { 305 });
	queue.next();
	check(queue.resolve(1, 1010) == EmojiResolveAdmission::Ready, "internal deadline makes failed ID retryable");
	queue.clear();
	check(!queue.current() && !queue.retryAt(), "revocation clears bounded queue state");
	auto entries = std::vector<EmojiExportEntry>();
	auto projected = 0;
	AppendBoundedEmojiExportEntries(entries, candidates, 100, [&](quint64 id) {
		++projected;
		return std::optional(EmojiExportEntry{ .id = id });
	});
	AppendBoundedEmojiExportEntries(entries, candidates, 100, [&](quint64 id) {
		++projected;
		return std::optional(EmojiExportEntry{ .id = id });
	});
	check(entries.size() == 100 && projected == 100, "recent and installed enumeration share candidate cap");
	entries.push_back({ .id = 999 });
	check(entries.size() == 101, "extra ID retains admission outside ordinary cap");

	check(!ClearEmojiExport({}), "empty cleanup directory refuses");
	check(!ClearEmojiExport(u"emoji-export"_q), "relative cleanup directory refuses");
	auto temp = QTemporaryDir();
	temp.setAutoRemove(false);
	check(temp.isValid(), "worker fixture directory");
	const auto directory = QFileInfo(temp.path()).canonicalFilePath() + u"/emoji-export"_q;
	auto worker = EmojiExportWorker(directory);
	const auto stamp = QDateTime::currentDateTimeUtc();
	auto image = QImage(128, 128, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	auto beats = 0;
	auto maximumGap = qint64(0);
	const auto wait = [&](const std::function<void(std::function<void(bool)>)> &launch) {
		auto loop = QEventLoop();
		auto timer = QTimer();
		auto timeout = QTimer();
		auto clock = QElapsedTimer();
		clock.start();
		auto previous = qint64(0);
		beats = 0;
		maximumGap = 0;
		auto done = false;
		auto ok = false;
		QObject::connect(&timer, &QTimer::timeout, [&] {
			const auto now = clock.elapsed();
			maximumGap = std::max(maximumGap, now - previous);
			previous = now;
			++beats;
		});
		QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
		timer.start(5);
		timeout.start(30000);
		launch([&](bool result) { ok = result; done = true; loop.quit(); });
		if (!done) loop.exec();
		check(done && ok, "worker operation completed");
	};
	wait([&](auto done) {
		check(worker.submit(worker.generation(), [=](const EmojiExportAuthority &current) {
			auto large = std::vector<EmojiExportEntry>();
			large.reserve(20000);
			if (!WriteEmojiExportPng(directory, 1, image, current)) return false;
			const auto first = directory + '/' + EmojiExportPngName(1);
			for (auto id = quint64(1); id <= 20000; ++id) {
				if (!current()) return false;
				large.push_back({ .id = id });
				if (id == 1) continue;
				const auto path = directory + '/' + EmojiExportPngName(id);
#ifdef Q_OS_WIN
				if (!QFile::copy(first, path)) return false;
#else
				if (::link(QFile::encodeName(first).constData(), QFile::encodeName(path).constData())) return false;
#endif
			}
			return PublishEmojiExportCatalog(directory, large, stamp, current);
		}, [=](quint64, bool ok) { done(ok); }), "large publication admitted");
	});
	std::printf("large publication heartbeat: beats=%d maximum_gap_ms=%lld\n", beats,
		static_cast<long long>(maximumGap));
	check(beats >= 5 && maximumGap < 150, "20k publication does not block main loop");
	check(QFileInfo::exists(directory + u"/catalog.json"_q), "worker published catalog");
	wait([&](auto done) { worker.revoke(true, [=](quint64, bool ok) { done(ok); }); });
	std::printf("large revocation heartbeat: beats=%d maximum_gap_ms=%lld\n", beats,
		static_cast<long long>(maximumGap));
	check(beats >= 5 && maximumGap < 150, "20k revocation does not block main loop");
	check(!QFileInfo::exists(directory + u"/catalog.json"_q)
		&& QDir(directory + u"/png"_q).entryList(QDir::Files).isEmpty(), "cleanup barrier removes owned output");
	auto automaticSlices = 0;
	wait([&](auto done) {
		const auto policy = std::make_shared<EmojiExportQueue>(100, 1000);
		const auto ids = std::make_shared<std::vector<quint64>>();
		for (auto id = quint64(1); id <= 100; ++id) ids->push_back(id);
		ids->push_back(999);
		policy->begin(*ids, { 999 });
		const auto pump = std::make_shared<std::function<void()>>();
		const auto weakPump = std::weak_ptr(pump);
		*pump = [&, policy, ids, weakPump, done] {
			const auto nextPump = weakPump.lock();
			if (!nextPump) return;
			const auto index = policy->current();
			if (!index) {
				check(worker.submit(worker.generation(), [=](const EmojiExportAuthority &current) {
					auto ready = std::vector<EmojiExportEntry>();
					for (const auto id : *ids) ready.push_back({ .id = id });
					return PublishEmojiExportCatalog(directory, ready, stamp, current);
				}, [=](quint64, bool ok) { done(ok); }), "automatic final catalog admitted");
				return;
			}
			const auto id = (*ids)[*index];
			if (policy->resolve(id, 0) == EmojiResolveAdmission::Continue) {
				++automaticSlices;
				QTimer::singleShot(1, &app, [=] { policy->continueSlice(); (*nextPump)(); });
				return;
			}
			check(worker.submit(worker.generation(), [=](const EmojiExportAuthority &current) {
				return WriteEmojiExportPng(directory, id, image, current);
			}, [=, &app](quint64, bool ok) {
				if (!ok) { done(false); return; }
				policy->next();
				QTimer::singleShot(0, &app, [=] { (*nextPump)(); });
			}), "automatic queued image admitted");
		};
		(*pump)();
	});
	check(automaticSlices == 1 && QFileInfo::exists(directory + '/' + EmojiExportPngName(999)),
		"extra PNG exported through automatic continuation without outside trigger");
	{
		auto file = QFile(directory + u"/catalog.json"_q);
		check(file.open(QIODevice::ReadOnly), "automatic catalog readable");
		const auto catalog = QJsonDocument::fromJson(file.readAll()).object();
		const auto array = catalog.value(u"entries"_q).toArray();
		check(array.size() == 101, "automatic continuation publishes full catalog");
		auto ordered = array.size() == 101;
		for (auto i = 0; i < array.size(); ++i) {
			const auto expected = i == 100 ? 999 : i + 1;
			ordered = ordered && array[i].toObject().value(u"id"_q).toString() == QString::number(expected);
		}
		check(ordered, "reserved extra processing preserves original catalog order");
	}
	wait([&](auto done) { worker.revoke(false, [=](quint64, bool ok) { done(ok); }); });
	check(QFileInfo::exists(directory + u"/catalog.json"_q), "ordinary quit barrier keeps cache");

	auto started = std::atomic<bool>(false);
	auto released = std::atomic<bool>(false);
	auto oldCompleted = false;
	auto oldResult = true;
	auto pendingCancelled = false;
	const auto old = worker.generation();
	check(worker.submit(old, [&](const EmojiExportAuthority &current) {
		started.store(true);
		while (!released.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		return WriteEmojiExportPng(directory, 7, image, current);
	}, [&](quint64, bool ok) { oldCompleted = true; oldResult = ok; }), "delayed old generation admitted");
	wait([&](auto done) {
		auto ready = new QTimer(&app);
		QObject::connect(ready, &QTimer::timeout, [&, ready, done] {
			if (!started.load()) return;
			ready->stop();
			ready->deleteLater();
			check(worker.submit(old, [](const EmojiExportAuthority&) { return true; },
				[&](quint64, bool ok) { pendingCancelled = !ok; }), "one pending worker job admitted");
			check(!worker.submit(old, [](const EmojiExportAuthority&) { return true; }, {}),
				"worker queue refuses a second pending job");
			worker.revoke(true, {});
			worker.revoke(false, [=](quint64, bool ok) { done(ok); });
			check(!worker.submit(old, [](const EmojiExportAuthority&) { return true; }, {}),
				"revoked generation cannot enqueue new output");
			released.store(true);
		});
		ready->start(1);
	});
	check(pendingCancelled, "revoke cancels pending generation");
	check(!worker.submit(old, [](const EmojiExportAuthority&) { return true; }, {}),
		"old generation still refused after cleanup barrier");
	check(oldCompleted && !oldResult, "revoked worker loses write authority");
	check(!QFileInfo::exists(directory + '/' + EmojiExportPngName(7)), "revoked worker cannot recreate after cleanup");
	check(!QFileInfo::exists(directory + u"/catalog.json"_q), "later no-clear barrier preserves earlier revocation requirement");
	std::printf("emoji async: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}

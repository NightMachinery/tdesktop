#include "purple/purple_emoji_large.h"
#include "purple/purple_emoji_large_store.h"
#include "purple/purple_emoji_export_worker.h"
#include "base/basic_types.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <atomic>
#include <cstdio>

namespace {

using namespace Purple;
auto checks = 0;
auto failures = 0;
auto cap = kEmojiLargeDefaultBytes;
qint64 EmojiLargeCacheCap() { return cap; }
void Check(bool value, const char *name) {
	++checks;
	if (!value) { ++failures; std::printf("FAIL: %s\n", name); }
}

class EmojiExporter final : public QObject {
public:
	explicit EmojiExporter(QString directory)
	: _directory(std::move(directory)), _worker(_directory, this) {
	}
	QString _directory;
	EmojiExportWorker _worker;
	quint64 _workerGeneration = _worker.generation();
	EmojiExportEpoch _epoch;
	quint64 _token = _epoch.begin();
	bool _rendering = false;
	bool _scanDue = true;
	bool _pruneLarge = true;
	bool _media = false;
	bool current = true;
	std::set<quint64> _acknowledge;
	std::vector<quint64> admittedIds;
	int pumps = 0;
	int errors = 0;
	int reconciles = 0;
	bool admitted() const { return current; }
	void reconcile() { ++reconciles; }
	void outputFailed() { ++errors; }
	void pump() { ++pumps; }
	void checkMedia() { ++pumps; }
	void admitRequests(const std::vector<quint64> &ids) { admittedIds = ids; }
	void maintainRequests();
	void capChanged();

};

#include "emoji-large-maintenance.inc"

}

int main(int argc, char **argv) {
	QCoreApplication app(argc, argv);
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	auto temporary = QTemporaryDir();
	if (!temporary.isValid()) return 2;
	const auto directory = QFileInfo(temporary.path()).canonicalFilePath() + u"/emoji-export"_q;
	auto e = EmojiExporter(directory);
	auto prepared = false;
	Check(e._worker.submit(e._workerGeneration, [&](const EmojiExportAuthority &current) {
		if (!EnsureEmojiLargeDirectories(directory)) return false;
		auto image = QImage(100, 100, QImage::Format_ARGB32_Premultiplied);
		image.fill(Qt::red);
		if (!WriteEmojiLargePng(directory, 1, image, {}, current)) return false;
		if (!WriteEmojiExportPng(directory, 1, EmojiExportFrame(image, {}), current)) return false;
		if (!PublishEmojiExportCatalog(directory, {{.id = 1, .large = EmojiLargePngName(1, 100),
			.sourceKind = u"webp"_q, .sourcePx = 100}}, QDateTime::currentDateTimeUtc(), current)) return false;
		for (const auto id : {1, 2}) {
			auto file = QFile(directory + u"/requests/"_q + QString::number(id));
			if (!file.open(QIODevice::WriteOnly)) return false;
		}
		return current();
	}, [&](quint64, bool ok) { prepared = ok; }), "prepare on real worker");
	const auto wait = [&](auto predicate) {
		auto elapsed = QElapsedTimer(); elapsed.start();
		while (!predicate() && elapsed.elapsed() < 10000) {
			app.processEvents(); QThread::msleep(1);
		}
		return predicate();
	};
	if (!wait([&] { return prepared; })) return 3;
	auto beats = 0;
	auto timer = QTimer();
	QObject::connect(&timer, &QTimer::timeout, [&] { ++beats; }); timer.start(1);
	e._acknowledge.insert(1);
	e.maintainRequests();
	Check(e._rendering && !e._scanDue, "maintenance owns only one worker slot");
	Check(wait([&] { return e.pumps == 1 || e.errors; }) && !e.errors,
		"actual maintenance completes and resumes main-loop owner");
	Check(e.admittedIds == std::vector<quint64>{2}, "acknowledged file removed before scanning later requests");

	auto blocked = std::atomic<bool>(false);
	auto release = std::atomic<bool>(false);
	Check(e._worker.submit(e._workerGeneration, [&](const EmojiExportAuthority &current) {
		blocked.store(true);
		while (!release.load() && current()) QThread::msleep(1);
		return current();
	}, {}), "controlled active job accepted");
	if (!wait([&] { return blocked.load(); })) return 4;
	e._scanDue = true; e._pruneLarge = false; e.maintainRequests();
	Check(e._rendering, "maintenance can wait behind bounded active job");
	cap = 0; e.capChanged();
	Check(e._scanDue && e._pruneLarge, "cap change during active job retains latest pruning obligation");
	release.store(true);
	Check(wait([&] { return e.pumps == 2 || e.errors; }), "pending maintenance settles");
	e.maintainRequests();
	Check(wait([&] { return e.pumps == 3 || e.errors; }) && !e.errors,
		"lowered cap is applied at next worker safe point");
	auto pruned = std::atomic<bool>(false);
	Check(e._worker.submit(e._workerGeneration, [&](const EmojiExportAuthority &) {
		pruned.store(!QFileInfo::exists(directory + '/' + EmojiLargePngName(1, 100))
			&& QFileInfo::exists(directory + '/' + EmojiExportPngName(1)));
		return pruned.load();
	}, [&](quint64, bool) {}), "inspect cache on worker");
	Check(wait([&] { return pruned.load(); }), "cap pruning keeps preview while removing large output");

	blocked.store(false); release.store(false);
	Check(e._worker.submit(e._workerGeneration, [&](const EmojiExportAuthority &current) {
		blocked.store(true); while (current() && !release.load()) QThread::msleep(1);
		return current();
	}, {}), "revocation interleaving active job");
	if (!wait([&] { return blocked.load(); })) return 5;
	e._scanDue = true; e.maintainRequests();
	const auto before = e.pumps;
	e._epoch.revoke(); e._token = e._epoch.begin();
	auto cleared = false;
	e._workerGeneration = e._worker.revoke(true, [&](quint64, bool ok) { cleared = ok; });
	e._rendering = true;
	Check(wait([&] { return cleared; }), "revocation cleanup barrier completes");
	Check(e._rendering && e.pumps == before && !e.errors,
		"retired maintenance callback cannot clear newer render or resume owner");
	Check(beats > 0, "main event loop remains responsive through maintenance and revocation");
	std::printf("emoji literal maintenance: %d checks, %d failures, %d heartbeat ticks\n", checks, failures, beats);
	return failures ? 1 : 0;
}

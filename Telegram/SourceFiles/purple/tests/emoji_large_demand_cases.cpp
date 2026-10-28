#include "purple/purple_emoji_export_queue.h"
#include "purple/purple_emoji_export_worker.h"
#include "purple/purple_emoji_large.h"
#include "purple/purple_emoji_large_store.h"
#include "base/basic_types.h"
#include <QtCore/QBuffer>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <algorithm>
#include <cstdio>

namespace {

using namespace Purple;
auto checks = 0;
auto failures = 0;
void Check(bool value, const char *name) {
	++checks;
	if (!value) { ++failures; std::printf("FAIL: %s\n", name); }
}
qint64 EmojiLargeCacheCap() { return kEmojiLargeDefaultBytes; }

class EmojiExporter final : public QObject {
public:
	explicit EmojiExporter(QString directory);
	void publish();
	void maintainRequests();
	void capChanged();
	bool admitted() const { return true; }
	void reconcile() {}
	void pump() {}
	void checkMedia() {}
	void outputFailed() { ++errors; }
	void admitRequests(const std::vector<quint64> &) {}

	QString _directory;
	EmojiExportWorker _worker;
	quint64 _workerGeneration = _worker.generation();
	EmojiExportEpoch _epoch;
	quint64 _token = _epoch.begin();
	EmojiExportQueue _queue{ 1 };
	EmojiSetTitleCache _setTitles;
	QElapsedTimer _clock;
	std::vector<std::optional<EmojiExportEntry>> _ready;
	std::shared_ptr<const std::vector<EmojiExportEntry>> _published
		= std::make_shared<const std::vector<EmojiExportEntry>>();
	std::set<quint64> _retiredDemandIds;
	std::set<quint64> _afterPublicationAck;
	std::set<quint64> _acknowledge;
	struct Timer { void start(int) {} } _debounce;
	bool _rendering = false;
	bool _publishPending = false;
	bool _pending = false;
	bool _running = false;
	bool _scanDue = false;
	bool _pruneLarge = false;
	bool _media = false;
	int errors = 0;

};

EmojiExporter::EmojiExporter(QString directory)
: _directory(std::move(directory))
, _worker(_directory, this) {
	_clock.start();
}

#include "emoji-large-publication.inc"
#include "emoji-large-maintenance.inc"

}

void RunDemandCohort(QCoreApplication &app, bool future) {
	auto temporary = QTemporaryDir();
	if (!temporary.isValid()) { Check(false, "temporary directory exists"); return; }
	const auto directory = QFileInfo(temporary.path()).canonicalFilePath() + u"/emoji-export"_q;
	auto e = EmojiExporter(directory);
	const auto wait = [&](auto predicate) {
		auto elapsed = QElapsedTimer();
		elapsed.start();
		while (!predicate() && elapsed.elapsed() < 30000) {
			app.processEvents();
			QThread::msleep(1);
		}
		return predicate();
	};
	const auto work = [&](EmojiExportWorker::Work action) {
		auto done = false;
		auto result = false;
		if (!e._worker.submit(e._workerGeneration, std::move(action), [&](quint64, bool ok) {
			result = ok;
			done = true;
		})) return false;
		return wait([&] { return done; }) && result;
	};
	auto image = QImage(100, 100, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::red);
	auto late = quint64(0);
	auto touched = QString();
	Check(work([&](const EmojiExportAuthority &current) {
		if (!EnsureEmojiLargeDirectories(directory)) return false;
		auto bytes = QByteArray();
		auto buffer = QBuffer(&bytes);
		if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) return false;
		const auto old = future ? QDateTime::currentDateTimeUtc().addSecs(3600)
			: QDateTime::fromSecsSinceEpoch(946684800, Qt::UTC);
		for (auto id = quint64(1); id <= kEmojiLargeMaximumFiles + 1; ++id) {
			auto file = QFile(directory + '/' + EmojiLargePngName(id, 100));
			if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()
				|| !file.flush() || !file.setFileTime(old, QFileDevice::FileModificationTime)) return false;
			file.close();
			if (QFileInfo(file).lastModified() != old) return false;
		}
		auto names = std::vector<QString>();
		auto iterator = QDirIterator(directory + u"/large"_q, QDir::Files);
		while (iterator.hasNext()) { iterator.next(); names.push_back(iterator.fileName()); }
		if (names.size() != kEmojiLargeMaximumFiles + 1) return false;
		late = *EmojiLargeRequestId(names.back().first(names.back().indexOf('-')));
		touched = *std::min_element(names.begin(), names.end() - 1);
		for (auto page = 0; page < 8; ++page) {
			if (!PruneEmojiLargeExport(directory, kEmojiLargeDefaultBytes, current)) return false;
			const auto stats = EmojiLargePruneStats();
			Check(stats.examined <= 256 && stats.removed <= 256 && stats.retained <= 2048,
				"partial LRU selection obeys page and index bounds");
		}
		Check(EmojiLargePruneStats().pending && EmojiLargePruneStats().retained == 2048,
			"full old cohort is retained before late candidate is examined");
		if ((!future && !TouchEmojiLargePng(directory, u"large/"_q + touched, current))
			|| !WriteEmojiLargePng(directory, late, image, {}, current)
			|| !WriteEmojiExportPng(directory, late, EmojiExportFrame(image, {}), current)) return false;
		auto request = QFile(directory + u"/requests/"_q + QString::number(late));
		if (!request.open(QIODevice::WriteOnly)) return false;
		auto pages = 0;
		do {
			if (!PruneEmojiLargeExport(directory, kEmojiLargeDefaultBytes, current)) return false;
			const auto stats = EmojiLargePruneStats();
			Check(stats.examined <= 256 && stats.removed <= 256 && stats.retained <= 2048,
				"late-file replacement keeps selection bounded");
		} while (EmojiLargePruneStats().pending && ++pages < 20);
		Check(QFileInfo::exists(directory + '/' + EmojiLargePngName(late, 100)),
			future ? "current demand survives a full future-dated cohort"
				: "newly demanded late file survives a full old cohort");
		Check(QFileInfo::exists(directory + u"/large/"_q + touched) != future,
			future ? "a future-dated file is evicted before the current demand"
				: "touch refreshes an already indexed candidate before selection");
		Check(QDir(directory + u"/large"_q).entryList(QDir::Files).size() == 2048,
			"an older file is evicted instead of growing the index or folder");
		return current();
	}), "prepare late demand and run real paged pruning on production worker");
	if (!late) return;
	const auto publish = [&](quint64 id) {
		e._ready = { EmojiExportEntry{ .id = id, .large = EmojiLargePngName(id, 100),
			.sourceKind = u"webp"_q, .sourcePx = 100 } };
		e._afterPublicationAck.insert(id);
		e._publishPending = true;
		e.publish();
		Check(wait([&] { return !e._rendering; }) && !e.errors, "literal production publication settles");
		e._scanDue = true;
		e.maintainRequests();
		Check(wait([&] { return !e._rendering; }) && !e.errors, "later maintenance settles");
	};
	publish(late);
	Check(work([&](const EmojiExportAuthority &) {
		auto file = QFile(directory + u"/catalog.json"_q);
		if (!file.open(QIODevice::ReadOnly)) return false;
		const auto entries = QJsonDocument::fromJson(file.readAll()).object().value(u"entries"_q).toArray();
		const auto entry = entries.isEmpty() ? QJsonObject() : entries[0].toObject();
		const auto relative = entry.value(u"large"_q).toString();
		return entry.value(u"id"_q).toString() == QString::number(late)
			&& relative == EmojiLargePngName(late, 100)
			&& QFileInfo(directory + '/' + relative).isFile()
			&& !QFileInfo::exists(directory + u"/requests/"_q + QString::number(late));
	}), future ? "future-cohort acknowledgement accompanies an advertised existing image"
		: "successful acknowledgement accompanies an advertised existing large image");
	const auto missing = quint64(999999);
	Check(work([&](const EmojiExportAuthority &current) {
		if (!WriteEmojiLargePng(directory, missing, image, {}, current)
			|| !WriteEmojiExportPng(directory, missing, EmojiExportFrame(image, {}), current)) return false;
		auto request = QFile(directory + u"/requests/"_q + QString::number(missing));
		return request.open(QIODevice::WriteOnly)
			&& QFile::remove(directory + '/' + EmojiLargePngName(missing, 100));
	}), "remove a rendered candidate before publication");
	publish(missing);
	Check(work([&](const EmojiExportAuthority &) {
		auto file = QFile(directory + u"/catalog.json"_q);
		if (!file.open(QIODevice::ReadOnly)) return false;
		const auto entries = QJsonDocument::fromJson(file.readAll()).object().value(u"entries"_q).toArray();
		return entries.size() == 1 && !entries[0].toObject().contains(u"large"_q)
			&& QFileInfo::exists(directory + u"/requests/"_q + QString::number(missing));
	}), "missing output is not advertised and its successful demand is not acknowledged");
}


template <typename Prune>
bool PruneAt(Prune prune, const QString &directory, qint64 cap,
		const Purple::EmojiExportAuthority &current, const QDateTime &now) {
	if constexpr (requires { prune(directory, cap, current, false, now); }) {
		return prune(directory, cap, current, false, now);
	} else {
		return prune(directory, cap, current, false);
	}
}

void RunClockBoundaries(QCoreApplication &app) {
	using namespace Purple;
	auto temporary = QTemporaryDir();
	if (!temporary.isValid()) { Check(false, "clock temporary directory exists"); return; }
	const auto root = QFileInfo(temporary.path()).canonicalFilePath();
	auto worker = EmojiExportWorker(root + u"/emoji-export"_q, &app);
	auto done = false;
	auto result = false;
	const auto submitted = worker.submit(worker.generation(), [&](const EmojiExportAuthority &current) {
		const auto now = QDateTime::fromMSecsSinceEpoch(1600000000000, Qt::UTC);
		auto image = QImage(100, 100, QImage::Format_ARGB32_Premultiplied);
		image.fill(Qt::red);
		for (const auto ties : { false, true }) {
			const auto directory = root + (ties ? u"/ties/emoji-export"_q : u"/boundary/emoji-export"_q);
			for (auto id = quint64(1); id != 4; ++id) {
				if (!WriteEmojiLargePng(directory, id, image, {}, current)) return false;
				auto file = QFile(directory + '/' + EmojiLargePngName(id, 100));
				const auto modified = ties ? now : id == 1 ? now.addSecs(300)
					: id == 2 ? now.addMSecs(300001) : now;
				if (!file.open(QIODevice::ReadOnly)
					|| !file.setFileTime(modified, QFileDevice::FileModificationTime)) return false;
				Check(QFileInfo(file).lastModified() == modified, "clock fixture preserves exact mtime");
			}
			const auto bytes = QFileInfo(directory + '/' + EmojiLargePngName(1, 100)).size();
			auto pages = 0;
			do {
				if (!PruneAt(&PruneEmojiLargeExport, directory, 2 * bytes, current, now)) return false;
				const auto stats = EmojiLargePruneStats();
				Check(stats.examined <= 256 && stats.removed <= 256 && stats.retained <= 2048,
					"clock ordering keeps page and index bounds");
			} while (EmojiLargePruneStats().pending && ++pages < 20);
			Check(!EmojiLargePruneStats().pending, "clock pruning completes");
			if (ties) {
				Check(!QFileInfo::exists(directory + '/' + EmojiLargePngName(1, 100))
					&& QFileInfo::exists(directory + '/' + EmojiLargePngName(2, 100))
					&& QFileInfo::exists(directory + '/' + EmojiLargePngName(3, 100)),
					"equal mtimes evict the lexicographically oldest filename");
			} else {
				Check(QFileInfo::exists(directory + '/' + EmojiLargePngName(1, 100)),
					"mtime exactly five minutes ahead remains plausible");
				Check(!QFileInfo::exists(directory + '/' + EmojiLargePngName(2, 100))
					&& QFileInfo::exists(directory + '/' + EmojiLargePngName(3, 100)),
					"mtime one millisecond beyond five minutes ranks older than current use");
			}
		}
		return current();
	}, [&](quint64, bool ok) { result = ok; done = true; });
	auto elapsed = QElapsedTimer();
	elapsed.start();
	while (submitted && !done && elapsed.elapsed() < 30000) {
		app.processEvents();
		QThread::msleep(1);
	}
	Check(submitted && done && result, "clock boundary and tie cases run on the production worker");
}

int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv);
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	RunDemandCohort(app, false);
	RunDemandCohort(app, true);
	RunClockBoundaries(app);
	std::printf("emoji large demand publication: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}

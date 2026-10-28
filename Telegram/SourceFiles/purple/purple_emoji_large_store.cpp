#include "purple/purple_emoji_large_store.h"

#include "base/basic_types.h"
#include "purple/purple_emoji_export_path.h"
#include "purple/purple_emoji_large.h"
#include <QtCore/QBuffer>
#include <QtCore/QHash>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtGui/QPainter>
#include <algorithm>
#include <array>
#include <limits>

namespace Purple {
namespace {

void RefreshLargePruneEntry(const QString &directory, const QString &relative);

bool Current(const std::function<bool()> &current) {
	return !current || current();
}

bool SafeDirectory(const QString &path, bool create) {
	if (!QDir::isAbsolutePath(path) || QDir::cleanPath(path) != path) return false;
	auto info = QFileInfo(path);
	while (!info.isRoot()) {
		if (info.isSymLink() || (info.exists() && !info.isDir())) return false;
		info = QFileInfo(info.absolutePath());
	}
	return QDir(path).exists() || (create && QDir().mkpath(path));
}

bool SafeExport(const QString &directory, bool create) {
	return QFileInfo(directory).fileName() == u"emoji-export"_q
		&& SafeDirectory(directory, create);
}

bool OwnedLargeName(const QString &name) {
	if (!name.endsWith(u".png"_q)) return false;
	const auto parts = name.chopped(4).split('-');
	if (parts.size() != 2 || !EmojiLargeRequestId(parts[0])) return false;
	auto ok = false;
	const auto pixels = parts[1].toInt(&ok);
	return ok && pixels > 0 && pixels <= kEmojiLargePixels
		&& QString::number(pixels) == parts[1];
}

bool OwnedLargePath(const QString &relative) {
	return relative.startsWith(u"large/"_q)
		&& OwnedLargeName(relative.sliced(6));
}

bool AtomicWrite(const QString &path, const QByteArray &bytes,
		const std::function<bool()> &current,
		std::optional<EmojiExportDirectoryIdentity> parent = {}) {
	if (!parent) parent = EmojiExportDirectoryId(QFileInfo(path).absolutePath());
	if (!parent || (current && !current()) || QFileInfo(path).isSymLink()
		|| !EmojiExportParentMatches(path, parent)) return false;
	auto file = QSaveFile(path);
	file.setDirectWriteFallback(false);
	return file.open(QIODevice::WriteOnly)
		&& file.write(bytes) == bytes.size()
		&& (!current || current())
		&& EmojiExportParentMatches(path, parent)
		&& file.commit();
}

bool RetractLargePaths(const QString &directory, const QSet<QString> &remove,
		const std::function<bool()> &current) {
	const auto path = directory + u"/catalog.json"_q;
	const auto parent = EmojiExportDirectoryId(directory);
	const auto info = QFileInfo(path);
	if (!info.exists()) return !info.isSymLink();
	if (!info.isFile() || info.isSymLink() || info.size() > 64 * 1024 * 1024) return false;
	auto file = QFile(path);
	if (!Current(current) || !file.open(QIODevice::ReadOnly)) return false;
	const auto document = QJsonDocument::fromJson(file.readAll());
	if (!document.isObject()) return false;
	auto object = document.object();
	if (object.value(u"version"_q).toInt() != 1
		|| !object.value(u"entries"_q).isArray()) return false;
	auto entries = object.value(u"entries"_q).toArray();
	if (entries.size() > 101025) return false;
	auto changed = false;
	for (auto i = 0; i != entries.size(); ++i) {
		if (!Current(current)) return false;
		auto entry = entries[i].toObject();
		if (!entry.contains(u"large"_q)) continue;
		const auto relative = entry.value(u"large"_q).toString();
		const auto large = QFileInfo(directory + '/' + relative);
		if (!OwnedLargePath(relative) || remove.contains(relative)
			|| !large.isFile() || large.isSymLink()) {
			entry.remove(u"large"_q);
			entries[i] = entry;
			changed = true;
		}
	}
	if (!changed) return Current(current);
	object.insert(u"entries"_q, entries);
	return AtomicWrite(path, QJsonDocument(object).toJson(QJsonDocument::Compact), current, parent);
}

}

QString EmojiLargePngName(quint64 id, int pixels) {
	return id && pixels > 0 && pixels <= kEmojiLargePixels
		? u"large/"_q + QString::number(id) + '-' + QString::number(pixels) + u".png"_q
		: QString();
}

bool EnsureEmojiLargeDirectories(const QString &directory) {
	return SafeExport(directory, true)
		&& SafeDirectory(directory + u"/large"_q, true)
		&& SafeDirectory(directory + u"/requests"_q, true);
}

bool WriteEmojiLargePng(const QString &directory, quint64 id, QImage image,
		std::optional<QColor> tint, const std::function<bool()> &current) {
	const auto pixels = std::max(image.width(), image.height());
	const auto name = EmojiLargePngName(id, pixels);
	if (!Current(current) || image.isNull() || name.isEmpty()
		|| !EnsureEmojiLargeDirectories(directory)) return false;
	image.setDevicePixelRatio(1.);
	image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	if (tint) {
		auto painter = QPainter(&image);
		painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
		painter.fillRect(image.rect(), *tint);
	}
	const auto parent = EmojiExportDirectoryId(directory + u"/large"_q);
	auto bytes = QByteArray();
	auto buffer = QBuffer(&bytes);
	const auto ok = buffer.open(QIODevice::WriteOnly)
		&& image.save(&buffer, "PNG")
		&& AtomicWrite(directory + '/' + name, bytes, current, parent);
	if (ok) RefreshLargePruneEntry(directory, name);
	return ok;
}

bool TouchEmojiLargePng(const QString &directory, const QString &relative,
		const std::function<bool()> &current) {
	if (!Current(current) || !SafeExport(directory, false)
		|| !SafeDirectory(directory + u"/large"_q, false)
		|| !OwnedLargePath(relative)) return false;
	const auto info = QFileInfo(directory + '/' + relative);
	if (!info.isFile() || info.isSymLink()) return false;
	auto file = QFile(info.absoluteFilePath());
	const auto ok = file.open(QIODevice::ReadOnly) && Current(current)
		&& file.setFileTime(QDateTime::currentDateTimeUtc(), QFileDevice::FileModificationTime);
	if (ok) RefreshLargePruneEntry(directory, relative);
	return ok;
}

namespace {

struct LargePruneState {
	QString directory;
	qint64 cap = -1;
	bool all = false;
	std::optional<EmojiExportDirectoryIdentity> parent;
	std::unique_ptr<QDirIterator> iterator;
	std::vector<QFileInfo> files;
	qint64 total = 0;
	bool removing = false;
	EmojiExportMaintenanceStats stats;
};

thread_local auto PruneState = LargePruneState();

bool NewerLargeFile(const QFileInfo &a, const QFileInfo &b, const QDateTime &now) {
	const auto plausibleUntil = now.addSecs(5 * 60);
	const auto rank = [&](const QFileInfo &file) {
		const auto modified = file.lastModified();
		return modified.isValid() && modified <= plausibleUntil
			? modified.toMSecsSinceEpoch() : std::numeric_limits<qint64>::min();
	};
	const auto first = rank(a);
	const auto second = rank(b);
	return first == second ? a.fileName() > b.fileName() : first > second;
}

void RefreshLargePruneEntry(const QString &directory, const QString &relative) {
	auto &state = PruneState;
	if (state.directory != directory
		|| EmojiExportDirectoryId(directory + u"/large"_q) != state.parent) return;
	const auto path = directory + '/' + relative;
	const auto entry = std::find_if(state.files.begin(), state.files.end(), [&](const auto &info) {
		return info.absoluteFilePath() == path;
	});
	if (entry == state.files.end()) return;
	const auto fresh = QFileInfo(path);
	state.total += fresh.size() - entry->size();
	*entry = fresh;
	const auto newer = [now = QDateTime::currentDateTimeUtc()](const auto &a, const auto &b) {
		return NewerLargeFile(a, b, now);
	};
	std::make_heap(state.files.begin(), state.files.end(), newer);
}

}

EmojiExportMaintenanceStats EmojiLargePruneStats() {
	return PruneState.stats;
}

bool PruneEmojiLargeExport(const QString &directory, qint64 maximumBytes,
		const std::function<bool()> &current, bool removeAll, QDateTime now) {
	auto &state = PruneState;
	state.stats = {};
	if (!Current(current) || maximumBytes < 0 || !SafeExport(directory, false)) {
		state = {};
		return false;
	}
	const auto large = directory + u"/large"_q;
	if (!QFileInfo::exists(large)) {
		state = {};
		return !QFileInfo(large).isSymLink();
	}
	const auto identity = EmojiExportDirectoryId(large);
	if (!identity) return false;
	if (state.directory != directory || state.cap != maximumBytes
		|| state.all != removeAll || state.parent != identity) state = {};
	if (state.directory.isEmpty()) {
		state.directory = directory;
		state.cap = maximumBytes;
		state.all = removeAll;
		state.parent = identity;
		state.iterator = std::make_unique<QDirIterator>(large,
			QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System | QDir::Hidden);
	}
	const auto newer = [now](const auto &a, const auto &b) {
		return NewerLargeFile(a, b, now);
	};
	std::make_heap(state.files.begin(), state.files.end(), newer);
	auto remove = QSet<QString>();
	while (!state.removing && state.stats.examined < kEmojiExportMaintenancePage
		&& state.iterator->hasNext()) {
		if (!Current(current)) { state = {}; return false; }
		state.iterator->next();
		++state.stats.examined;
		const auto info = QFileInfo(state.iterator->filePath());
		if (!OwnedLargeName(info.fileName()) || (!info.isFile() && !info.isSymLink())) continue;
		if (removeAll || info.isSymLink()) {
			remove.insert(u"large/"_q + info.fileName());
		} else if (state.files.size() < kEmojiLargeMaximumFiles) {
			state.files.push_back(info);
			state.total += info.size();
			std::push_heap(state.files.begin(), state.files.end(), newer);
		} else if (newer(info, state.files.front())) {
			std::pop_heap(state.files.begin(), state.files.end(), newer);
			const auto &oldest = state.files.back();
			remove.insert(u"large/"_q + oldest.fileName());
			state.total += info.size() - oldest.size();
			state.files.back() = info;
			std::push_heap(state.files.begin(), state.files.end(), newer);
		} else {
			remove.insert(u"large/"_q + info.fileName());
		}
	}
	if (!state.removing && !state.iterator->hasNext()) {
		state.iterator.reset();
		state.removing = true;
	}
	while (state.removing && state.total > maximumBytes
		&& !state.files.empty()
		&& remove.size() < kEmojiExportMaintenancePage) {
		if (!Current(current)) { state = {}; return false; }
		std::pop_heap(state.files.begin(), state.files.end(), newer);
		const auto &info = state.files.back();
		remove.insert(u"large/"_q + info.fileName());
		state.total -= info.size();
		state.files.pop_back();
	}
	if (!remove.isEmpty() && !RetractLargePaths(directory, remove, current)) return false;
	for (const auto &relative : remove) {
		if (!RemoveEmojiExportFile(directory + '/' + relative, state.parent, current)) return false;
		++state.stats.removed;
	}
	state.stats.retained = state.files.size();
	state.stats.pending = !state.removing || state.total > maximumBytes;
	if (!state.stats.pending) {
		const auto stats = state.stats;
		state = {};
		state.stats = stats;
	}
	return Current(current);
}

bool AcknowledgeEmojiLargeRequest(const QString &directory, quint64 id,
		const std::function<bool()> &current) {
	if (!Current(current) || !id || !SafeExport(directory, false)
		|| !SafeDirectory(directory + u"/requests"_q, false)) return false;
	const auto file = QFileInfo(directory + u"/requests/"_q + QString::number(id));
	const auto parent = EmojiExportDirectoryId(file.absolutePath());
	return !file.exists() ? !file.isSymLink()
		: file.isFile() && !file.isSymLink() && file.size() == 0
			&& RemoveEmojiExportFile(file.absoluteFilePath(), parent, current);
}

struct EmojiExportCleanup::State {
	QString directory;
	int phase = 0;
	std::optional<EmojiExportDirectoryIdentity> parent;
	std::unique_ptr<QDirIterator> iterator;
	EmojiExportMaintenanceStats stats;
};

EmojiExportCleanup::EmojiExportCleanup() : _state(std::make_unique<State>()) {
}

EmojiExportCleanup::~EmojiExportCleanup() = default;

EmojiExportMaintenanceStats EmojiExportCleanup::stats() const {
	return _state->stats;
}

bool EmojiExportCleanupRecorded(const QString &directory) {
	return QFileInfo::exists(directory + u"/.cleanup-pending"_q);
}

bool RecordEmojiExportCleanup(const QString &directory) {
	if (!QFileInfo::exists(directory)) return true;
	return SafeExport(directory, false)
		&& AtomicWrite(directory + u"/.cleanup-pending"_q, "1", {});
}

EmojiExportCleanupResult EmojiExportCleanup::step(const QString &directory,
		const std::function<bool()> &current) {
	auto &state = *_state;
	state.stats = {};
	if (!Current(current) || !QDir::isAbsolutePath(directory)
		|| QDir::cleanPath(directory) != directory
		|| QFileInfo(directory).fileName() != u"emoji-export"_q) {
		return EmojiExportCleanupResult::Failed;
	}
	if (!QFileInfo::exists(directory) && !QFileInfo(directory).isSymLink()) {
		return EmojiExportCleanupResult::Done;
	}
	if (!SafeExport(directory, false)) return EmojiExportCleanupResult::Failed;
	if (state.directory != directory) {
		state = {};
		state.directory = directory;
	}
	const auto root = EmojiExportDirectoryId(directory);
	if (!state.phase) {
		PruneState = {};
		if (!RecordEmojiExportCleanup(directory)) return EmojiExportCleanupResult::Failed;
		const auto catalog = directory + u"/catalog.json"_q;
		if ((QFileInfo::exists(catalog) || QFileInfo(catalog).isSymLink())
			&& !RemoveEmojiExportFile(catalog, root, current)) return EmojiExportCleanupResult::Failed;
		state.phase = 1;
	}
	const auto folders = std::array{ u"png"_q, u"large"_q, u"requests"_q };
	while (state.phase <= folders.size()) {
		const auto path = directory + '/' + folders[state.phase - 1];
		if (!state.iterator) {
			if (!QFileInfo::exists(path) && !QFileInfo(path).isSymLink()) { ++state.phase; continue; }
			state.parent = EmojiExportDirectoryId(path);
			if (!state.parent) return EmojiExportCleanupResult::Failed;
			state.iterator = std::make_unique<QDirIterator>(path,
				QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System | QDir::Hidden);
		}
		if (EmojiExportDirectoryId(path) != state.parent) return EmojiExportCleanupResult::Failed;
		while (state.stats.examined < kEmojiExportMaintenancePage && state.iterator->hasNext()) {
			if (!Current(current)) return EmojiExportCleanupResult::Failed;
			state.iterator->next();
			++state.stats.examined;
			const auto info = state.iterator->fileInfo();
			const auto name = info.fileName();
			const auto owned = (state.phase == 1)
				? name.endsWith(u"-128.png"_q) && EmojiLargeRequestId(name.chopped(8)).has_value()
				: (state.phase == 2) ? OwnedLargeName(name)
				: info.isFile() && !info.isSymLink() && !info.size();
			if (owned && (info.isFile() || info.isSymLink())) {
				if (!RemoveEmojiExportFile(info.absoluteFilePath(), state.parent, current)) {
					return EmojiExportCleanupResult::Failed;
				}
				++state.stats.removed;
			}
		}
		if (state.iterator->hasNext()) {
			state.stats.pending = true;
			return EmojiExportCleanupResult::Pending;
		}
		state.iterator.reset();
		++state.phase;
		if (state.stats.examined == kEmojiExportMaintenancePage && state.phase <= folders.size()) {
			state.stats.pending = true;
			return EmojiExportCleanupResult::Pending;
		}
	}
	const auto marker = directory + u"/.cleanup-pending"_q;
	if (!RemoveEmojiExportFile(marker, root, current)) return EmojiExportCleanupResult::Failed;
	return EmojiExportCleanupResult::Done;
}

std::vector<EmojiExportEntry> MergeEmojiLargePublication(
		const std::vector<EmojiExportEntry> &previous,
		const std::vector<std::optional<EmojiExportEntry>> &ready,
		bool finished,
		const std::function<bool()> &current,
		const std::set<quint64> &retired) {
	auto result = finished ? std::vector<EmojiExportEntry>() : previous;
	std::erase_if(result, [&](const auto &entry) { return retired.contains(entry.id); });
	auto positions = QHash<quint64, size_t>();
	for (auto i = size_t(0); i != result.size(); ++i) {
		if (!Current(current)) return {};
		positions.insert(result[i].id, i);
	}
	auto titles = QHash<quint64, EmojiExportEntry>();
	for (const auto &entry : ready) {
		if (!Current(current)) return {};
		if (!entry) continue;
		const auto existing = positions.constFind(entry->id);
		if (existing == positions.cend()) {
			positions.insert(entry->id, result.size());
			result.push_back(*entry);
		} else {
			result[*existing] = *entry;
		}
		if (entry->setId && !entry->setTitle.isEmpty()) titles.insert(entry->setId, *entry);
	}
	for (auto &entry : result) {
		if (!Current(current)) return {};
		const auto title = titles.constFind(entry.setId);
		if (title == titles.cend()) continue;
		entry.setTitle = title->setTitle;
		entry.setShortName = title->setShortName;
		entry.installed = title->installed;
	}
	return result;
}

bool PublishEmojiLargeRequests(const QString &directory,
		std::vector<EmojiExportEntry> &entries, const QDateTime &generatedAt,
		const std::set<quint64> &acknowledge, const std::function<bool()> &current) {
	ValidateEmojiLargeEntries(directory, entries);
	if (!PublishEmojiExportCatalog(directory, entries, generatedAt, current)) return false;
	for (const auto &entry : entries) {
		if (!Current(current)) return false;
		if (!acknowledge.contains(entry.id) || entry.large.isEmpty()) continue;
		const auto usable = [&] {
			if (!Current(current) || !SafeDirectory(directory + u"/large"_q, false)
				|| !entry.sourcePx || entry.large != EmojiLargePngName(entry.id, *entry.sourcePx)) return false;
			const auto file = QFileInfo(directory + '/' + entry.large);
			return file.isFile() && !file.isSymLink() && file.size() > 0;
		};
		if (usable()) static_cast<void>(AcknowledgeEmojiLargeRequest(directory, entry.id, usable));
	}
	return Current(current);
}

void ValidateEmojiLargeEntries(const QString &directory,
		std::vector<EmojiExportEntry> &entries) {
	const auto safe = SafeDirectory(directory + u"/large"_q, false);
	for (auto &entry : entries) {
		if (entry.large.isEmpty()) continue;
		const auto info = QFileInfo(directory + '/' + entry.large);
		if (!safe || !entry.sourcePx
			|| entry.large != EmojiLargePngName(entry.id, *entry.sourcePx)
			|| !info.isFile() || info.isSymLink()) entry.large.clear();
	}
}

std::vector<quint64> ScanEmojiLargeRequests(const QString &directory,
		quint64 generation, const std::function<bool()> &current) {
	thread_local auto scanner = EmojiLargeRequestScanner();
	thread_local auto previousGeneration = quint64(0);
	if (previousGeneration != generation) {
		scanner.reset();
		previousGeneration = generation;
	}
	return scanner.scan(directory, current);
}

EmojiLargeRequestScanner::EmojiLargeRequestScanner() = default;
EmojiLargeRequestScanner::~EmojiLargeRequestScanner() = default;

std::vector<quint64> EmojiLargeRequestScanner::scan(const QString &directory,
		const std::function<bool()> &current, size_t examined, size_t admitted) {

	auto result = std::vector<quint64>();
	_stats = {};
	if (!Current(current) || !EnsureEmojiLargeDirectories(directory)) return result;
	if (_directory != directory) reset();
	_directory = directory;
	const auto path = directory + u"/requests"_q;
	const auto parent = EmojiExportDirectoryId(path);
	if (!parent) return result;
	if (!_iterator) {
		_iterator = std::make_unique<QDirIterator>(path,
			QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System | QDir::Hidden);
	}
	const auto budget = std::min(examined, kEmojiExportMaintenancePage);
	const auto maximum = std::min(admitted, size_t(64));
	auto malformed = std::vector<QFileInfo>();
	while (_stats.examined < budget && (_counting || result.size() < maximum) && _iterator->hasNext()) {
		if (!Current(current)) { reset(); return {}; }
		_iterator->next();
		++_stats.examined;
		const auto info = _iterator->fileInfo();
		_entries = std::min(_entries + 1, kEmojiLargeMaximumRequests + 1);
		if (_entries > kEmojiLargeMaximumRequests) _overflow = true;
		if (info.isSymLink() || !info.isFile() || info.size() != 0) continue;
		const auto id = EmojiLargeRequestId(info.fileName());
		if (!id) malformed.push_back(info);
		else if (!_overflow && result.size() < maximum) result.push_back(*id);
	}
	if (_overflow) {
		result.clear();
		std::sort(malformed.begin(), malformed.end(), [](const auto &a, const auto &b) {
			return a.lastModified() == b.lastModified()
				? a.fileName() < b.fileName() : a.lastModified() < b.lastModified();
		});
		for (const auto &info : malformed) {
			if (!RemoveEmojiExportFile(info.absoluteFilePath(), parent, current)) { reset(); return {}; }
			++_stats.removed;
		}
	}
	_stats.retained = malformed.size();
	_stats.pending = _iterator->hasNext();
	if (_counting && _stats.pending) result.clear();
	if (!_stats.pending) {
		const auto withinLimit = _entries <= kEmojiLargeMaximumRequests;
		_counting = _overflow || !_counting;
		if (_overflow && withinLimit) { _overflow = false; _counting = true; }
		_entries = 0;
		_iterator.reset();
	}
	return result;
}

EmojiExportMaintenanceStats EmojiLargeRequestScanner::stats() const {
	return _stats;
}

bool EmojiLargeRequestScanner::overflow() const {
	return _overflow;
}

void EmojiLargeRequestScanner::reset() {
	_iterator.reset();
	_directory.clear();
	_entries = 0;
	_overflow = false;
	_counting = true;
	_stats = {};
}

}

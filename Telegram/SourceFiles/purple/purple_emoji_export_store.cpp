#include "purple/purple_emoji_export_store.h"

#include "base/basic_types.h"

#include <QtCore/QBuffer>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtGui/QPainter>

namespace Purple {
namespace {

bool SafeDirectory(const QString &path, bool create) {
	if (!QDir::isAbsolutePath(path) || QDir::cleanPath(path) != path) {
		return false;
	}
	auto info = QFileInfo(path);
	while (!info.isRoot()) {
		if (info.isSymLink() || (info.exists() && !info.isDir())) {
			return false;
		}
		info = QFileInfo(info.absolutePath());
	}
	return QDir(path).exists() || (create && QDir().mkpath(path));
}

bool SafeExport(const QString &directory, bool create) {
	return QFileInfo(directory).fileName() == u"emoji-export"_q
		&& SafeDirectory(directory, create)
		&& SafeDirectory(directory + u"/png"_q, create);
}

bool AtomicWrite(const QString &path, const QByteArray &bytes,
		const std::function<bool()> &current) {
	if ((current && !current()) || QFileInfo(path).isSymLink()) {
		return false;
	}
	auto file = QSaveFile(path);
	file.setDirectWriteFallback(false);
	return file.open(QIODevice::WriteOnly)
		&& file.write(bytes) == bytes.size()
		&& (!current || current())
		&& file.commit();
}

bool OwnedPng(const QString &name) {
	const auto suffix = u"-128.png"_q;
	if (!name.endsWith(suffix)) {
		return false;
	}
	const auto number = name.chopped(suffix.size());
	auto ok = false;
	const auto id = number.toULongLong(&ok);
	return ok && id && QString::number(id) == number;
}

bool RemoveStale(const QString &directory, const QSet<QString> &keep,
		const std::function<bool()> &current) {
	auto ok = true;
	const auto png = QDir(directory + u"/png"_q);
	for (const auto &info : png.entryInfoList(QDir::Files | QDir::System | QDir::Hidden)) {
		if (current && !current()) return false;
		const auto name = info.fileName();
		if (OwnedPng(name) && !keep.contains(name)) {
			ok = QFile::remove(info.absoluteFilePath()) && ok;
		}
	}
	return ok;
}

}

quint64 EmojiExportEpoch::begin() {
	_active = true;
	return ++_generation;
}

void EmojiExportEpoch::revoke() {
	_active = false;
	++_generation;
}

bool EmojiExportEpoch::accepts(quint64 token) const {
	return _active && token == _generation;
}

QString EmojiExportDirectory(
		const QString &home,
		const QString &workdir,
		const QString &defaultWorkdir) {
	if (!QDir::isAbsolutePath(home) || !QDir::isAbsolutePath(workdir)) {
		return {};
	}
	const auto working = QDir::cleanPath(workdir);
	return (QDir::isAbsolutePath(defaultWorkdir)
		&& working == QDir::cleanPath(defaultWorkdir))
		? QDir::cleanPath(home) + u"/Library/Caches/Purple/emoji-export"_q
		: working + u"/emoji-export"_q;
}

QString EmojiExportPngName(quint64 id) {
	return u"png/"_q + QString::number(id)
		+ u"-128.png"_q;
}

std::vector<EmojiExportEntry> UniqueEmojiExportEntries(
		const std::vector<EmojiExportEntry> &entries) {
	auto result = std::vector<EmojiExportEntry>();
	auto seen = QSet<quint64>();
	for (auto entry : entries) {
		if (!entry.id || seen.contains(entry.id)) {
			continue;
		}
		seen.insert(entry.id);
		if (entry.recentRank && *entry.recentRank <= 0) {
			entry.recentRank = std::nullopt;
		}
		result.push_back(std::move(entry));
	}
	return result;
}

QJsonObject EmojiExportCatalog(
		const std::vector<EmojiExportEntry> &entries,
		const QDateTime &generatedAt) {
	auto array = QJsonArray();
	for (const auto &entry : UniqueEmojiExportEntries(entries)) {
		array.push_back(QJsonObject{
			{ u"id"_q, QString::number(entry.id) },
			{ u"alt"_q, entry.alt },
			{ u"setId"_q, QString::number(entry.setId) },
			{ u"setTitle"_q, entry.setTitle },
			{ u"setShortName"_q, entry.setShortName },
			{ u"textColor"_q, entry.textColor },
			{ u"recentRank"_q, entry.recentRank
				? QJsonValue(*entry.recentRank) : QJsonValue(QJsonValue::Null) },
			{ u"png"_q, EmojiExportPngName(entry.id) },
		});
	}
	return {
		{ u"version"_q, 1 },
		{ u"generatedAt"_q, generatedAt.toUTC().toString(Qt::ISODateWithMs) },
		{ u"entries"_q, array },
	};
}

QImage EmojiExportFrame(QImage frame, std::optional<QColor> tint) {
	if (frame.isNull()) {
		return {};
	}
	frame.setDevicePixelRatio(1.);
	if (frame.width() > kEmojiExportPixels || frame.height() > kEmojiExportPixels) {
		frame = frame.scaled(kEmojiExportPixels, kEmojiExportPixels,
			Qt::KeepAspectRatio, Qt::SmoothTransformation);
	}
	if (tint) {
		frame = frame.convertToFormat(QImage::Format_ARGB32_Premultiplied);
		auto painter = QPainter(&frame);
		painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
		painter.fillRect(frame.rect(), *tint);
	}
	auto image = QImage(kEmojiExportPixels, kEmojiExportPixels,
		QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	auto painter = QPainter(&image);
	painter.drawImage((kEmojiExportPixels - frame.width()) / 2,
		(kEmojiExportPixels - frame.height()) / 2, frame);
	return image;
}

bool WriteEmojiExportPng(
		const QString &directory,
		quint64 id,
		const QImage &image,
		const std::function<bool()> &current) {
	if ((current && !current()) || !id || image.size() != QSize(kEmojiExportPixels, kEmojiExportPixels)
		|| !SafeExport(directory, true)) {
		return false;
	}
	auto bytes = QByteArray();
	auto buffer = QBuffer(&bytes);
	return buffer.open(QIODevice::WriteOnly)
		&& image.save(&buffer, "PNG")
		&& AtomicWrite(directory + '/' + EmojiExportPngName(id), bytes, current);
}

bool PublishEmojiExportCatalog(
		const QString &directory,
		const std::vector<EmojiExportEntry> &entries,
		const QDateTime &generatedAt,
		const std::function<bool()> &current) {
	if ((current && !current()) || !generatedAt.isValid() || !SafeExport(directory, true)) {
		return false;
	}
	auto keep = QSet<QString>();
	for (const auto &entry : UniqueEmojiExportEntries(entries)) {
		if (current && !current()) return false;
		const auto relative = EmojiExportPngName(entry.id);
		const auto info = QFileInfo(directory + '/' + relative);
		if (!info.isFile() || info.isSymLink()) {
			return false;
		}
		keep.insert(info.fileName());
	}
	if (!AtomicWrite(directory + u"/catalog.json"_q,
		QJsonDocument(EmojiExportCatalog(entries, generatedAt)).toJson(QJsonDocument::Compact), current)) {
		return false;
	}
	return RemoveStale(directory, keep, current);
}

bool ClearEmojiExport(const QString &directory,
		const std::function<bool()> &current) {
	if ((current && !current()) || !QDir::isAbsolutePath(directory)
		|| QDir::cleanPath(directory) != directory
		|| QFileInfo(directory).fileName() != u"emoji-export"_q) {
		return false;
	}
	if (!QFileInfo::exists(directory)) {
		return true;
	}
	if (!SafeDirectory(directory, false)
		|| QFileInfo(directory).fileName() != u"emoji-export"_q) {
		return false;
	}
	const auto catalog = directory + u"/catalog.json"_q;
	if ((QFileInfo::exists(catalog) || QFileInfo(catalog).isSymLink())
		&& !QFile::remove(catalog)) {
		return false;
	}
	const auto png = directory + u"/png"_q;
	return !QFileInfo::exists(png)
		|| (SafeDirectory(png, false) && RemoveStale(directory, {}, current));
}

}

#include "purple/purple_emoji_export_store.h"
#include "base/basic_types.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <cstdio>
#include <limits>

int main(int argc, char **argv) {
	using namespace Purple;
	auto app = QCoreApplication(argc, argv);
	auto checks = 0;
	auto failures = 0;
	const auto check = [&](bool value, const char *name) {
		++checks;
		if (!value) {
			++failures;
			std::printf("FAIL %s\n", name);
		}
	};
	const auto write = [](const QString &path, const QByteArray &bytes) {
		auto file = QFile(path);
		return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
	};
	const auto read = [](const QString &path) {
		auto file = QFile(path);
		return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
	};
	const auto stamp = QDateTime::fromString(u"2026-01-02T03:04:05Z"_q, Qt::ISODate);
	const auto maximum = std::numeric_limits<quint64>::max();
	auto entries = std::vector<EmojiExportEntry>{
		{ .id = 20, .alt = u"x"_q, .setId = maximum,
			.setTitle = u"Fixture set"_q, .setShortName = u"fixture_set"_q,
			.textColor = true, .recentRank = 1 },
		{ .id = 10, .recentRank = 3 },
		{ .id = 20, .recentRank = 8 },
		{ .id = maximum },
		{ .id = 30, .recentRank = 0 },
		{ .id = 0 },
	};
	const auto unique = UniqueEmojiExportEntries(entries);
	check(unique.size() == 4, "dedup and zero exclusion");
	check(unique[0].id == 20 && unique[1].id == 10
		&& unique[2].id == maximum && unique[3].id == 30, "client order retained");
	check(unique[0].recentRank == 1, "first seen recent rank retained");
	check(!unique[3].recentRank, "nonpositive rank becomes null");
	const auto catalog = EmojiExportCatalog(entries, stamp);
	check(catalog.value(u"version"_q).toInt() == 1, "version one");
	check(catalog.value(u"generatedAt"_q).toString() == u"2026-01-02T03:04:05.000Z"_q,
		"UTC ISO timestamp");
	const auto array = catalog.value(u"entries"_q).toArray();
	check(array.size() == 4, "JSON deduplicated");
	const auto first = array[0].toObject();
	check(first.value(u"id"_q).isString() && first.value(u"id"_q).toString() == u"20"_q,
		"document IDs are decimal strings");
	check(first.value(u"setId"_q).isString()
		&& first.value(u"setId"_q).toString() == u"18446744073709551615"_q,
		"set ID retains all 64 bits");
	check(array[2].toObject().value(u"id"_q).toString() == u"18446744073709551615"_q,
		"document ID retains all 64 bits");
	check(first.value(u"alt"_q).toString() == u"x"_q
		&& first.value(u"setTitle"_q).toString() == u"Fixture set"_q
		&& first.value(u"setShortName"_q).toString() == u"fixture_set"_q,
		"library metadata fields");
	check(first.value(u"textColor"_q).toBool(), "template flag");
	check(first.value(u"recentRank"_q).toInt() == 1, "positive rank");
	check(array[2].toObject().value(u"recentRank"_q).isNull(), "installed rank null");
	check(first.value(u"png"_q).toString() == u"png/20-128.png"_q, "relative PNG path");
	check(EmojiExportDirectory(u"/home/example"_q, u"/app/default/"_q, u"/app/default"_q)
		== u"/home/example/Library/Caches/Purple/emoji-export"_q, "default workdir path");
	check(EmojiExportDirectory(u"/home/example"_q, u"/app/custom"_q, u"/app/default"_q)
		== u"/app/custom/emoji-export"_q, "other workdir isolated");
	check(EmojiExportDirectory(u"/home/example"_q, u"/app/custom"_q, {}).endsWith(
		u"/app/custom/emoji-export"_q), "unknown default stays isolated");
	check(EmojiExportDirectory(u"relative"_q, u"/app/custom"_q, {}).isEmpty(),
		"relative home refuses");
	check(EmojiExportDirectory(u"/home/example"_q, u"relative"_q, {}).isEmpty(),
		"relative workdir refuses");
	auto epoch = EmojiExportEpoch();
	check(!epoch.accepts(0), "initial generation refuses");
	const auto opening = epoch.begin();
	check(epoch.accepts(opening), "current generation admits");
	epoch.revoke();
	check(!epoch.accepts(opening), "disable lock logout switch revoke old completion");
	const auto newer = epoch.begin();
	check(epoch.accepts(newer) && !epoch.accepts(opening), "new session rejects prior worker");
	auto source = QImage(64, 32, QImage::Format_ARGB32_Premultiplied);
	source.fill(QColor(20, 40, 60, 128));
	const auto image = EmojiExportFrame(source, std::nullopt);
	check(image.size() == QSize(128, 128) && image.devicePixelRatio() == 1., "fixed output canvas");
	check(image.pixelColor(0, 0).alpha() == 0, "transparent canvas");
	check(image.pixelColor(64, 64).alpha() == 128, "source alpha retained");
	const auto tint = EmojiExportFrame(source, QColor(240, 80, 20));
	const auto pixel = tint.pixelColor(64, 64);
	check(pixel.alpha() == 128 && pixel.red() >= 238 && pixel.green() >= 78
		&& pixel.blue() >= 18, "templates tinted without losing alpha");
	check(EmojiExportFrame({}, std::nullopt).isNull(), "failed render remains absent");
	auto big = QImage(512, 256, QImage::Format_ARGB32_Premultiplied);
	big.fill(Qt::red);
	const auto fitted = EmojiExportFrame(big, std::nullopt);
	check(fitted.pixelColor(64, 32).alpha() == 255
		&& fitted.pixelColor(64, 31).alpha() == 0, "oversize aspect ratio fitted");
	auto temp = QTemporaryDir();
	check(temp.isValid(), "file fixture");
	const auto directory = QFileInfo(temp.path()).canonicalFilePath() + u"/emoji-export"_q;
	const auto path = directory + u"/catalog.json"_q;
	const auto png20 = directory + '/' + EmojiExportPngName(20);
	const auto png10 = directory + '/' + EmojiExportPngName(10);
	const auto firstOnly = std::vector<EmojiExportEntry>{ entries[0] };
	check(ClearEmojiExport(directory), "absent export cleanup");
	check(!PublishEmojiExportCatalog(directory, firstOnly, stamp), "catalog refuses missing image");
	check(!QFileInfo::exists(path), "catalog never precedes PNG");
	check(!WriteEmojiExportPng(directory, 0, image), "zero document refuses");
	check(!WriteEmojiExportPng(directory, 20, source), "incorrect dimension refuses");
	check(WriteEmojiExportPng(directory, 20, image), "atomic PNG write");
	check(QImage(png20).size() == QSize(128, 128), "saved PNG decodes at contract size");
	check(!QFileInfo::exists(path), "PNG write alone does not publish catalog");
	check(PublishEmojiExportCatalog(directory, firstOnly, stamp), "catalog published after PNG");
	const auto original = read(path);
	check(QJsonDocument::fromJson(original).object() == EmojiExportCatalog(firstOnly, stamp),
		"published catalog complete");
	check(!PublishEmojiExportCatalog(directory, unique, stamp), "incomplete refresh refuses");
	check(read(path) == original && QFileInfo::exists(png20), "failed refresh preserves old catalog and image");
	check(write(directory + u"/png/notes.txt"_q, "fixture"), "unrelated file fixture");
	check(write(directory + u"/png/020-128.png"_q, "fixture"), "noncanonical file fixture");
	check(WriteEmojiExportPng(directory, 10, tint), "replacement PNG ready");
	check(PublishEmojiExportCatalog(directory, { entries[1] }, stamp), "replacement catalog committed");
	check(!QFileInfo::exists(png20) && QFileInfo::exists(png10), "stale owned PNG removed after commit");
	check(QFileInfo::exists(directory + u"/png/notes.txt"_q)
		&& QFileInfo::exists(directory + u"/png/020-128.png"_q), "unowned files preserved");
	check(PublishEmojiExportCatalog(directory, {}, stamp), "empty library catalog valid");
	check(!QFileInfo::exists(png10), "empty library removes stale artwork");
	check(WriteEmojiExportPng(directory, 20, image), "cleanup image fixture");
	check(ClearEmojiExport(directory), "revocation removes output");
	check(!QFileInfo::exists(path) && !QFileInfo::exists(png20), "catalog and owned images revoked");
	check(QFileInfo::exists(directory + u"/png/notes.txt"_q), "cleanup preserves unowned file");
	check(!WriteEmojiExportPng(directory + u"/../elsewhere"_q, 20, image), "traversal refuses");
	check(!PublishEmojiExportCatalog(directory, {}, QDateTime()), "invalid timestamp refuses");
	const auto linked = QFileInfo(temp.path()).canonicalFilePath() + u"/linked"_q;
	check(QDir().mkpath(linked), "symlink parent fixture");
	check(QFile::link(directory, linked + u"/emoji-export"_q), "export root symlink fixture");
	check(!WriteEmojiExportPng(linked + u"/emoji-export"_q, 20, image), "symlink root refuses");
	check(!ClearEmojiExport(linked + u"/emoji-export"_q), "cleanup never follows symlink root");
	check(QFile::remove(directory + u"/png/notes.txt"_q)
		&& QFile::remove(directory + u"/png/020-128.png"_q)
		&& QDir().rmdir(directory + u"/png"_q), "prepare png symlink fixture");
	check(QFile::link(linked, directory + u"/png"_q), "PNG directory symlink fixture");
	check(!WriteEmojiExportPng(directory, 20, image), "symlink PNG directory refuses");
	check(!QFileInfo::exists(linked + u"/20-128.png"_q), "no outside write");
	std::printf("emoji export: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}

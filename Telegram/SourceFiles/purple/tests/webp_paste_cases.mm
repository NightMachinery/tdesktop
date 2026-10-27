#include "purple/purple_webp_paste.h"
#include "purple/purple_webp_paste_mac.h"
#include "purple/purple_webp_paste_route.h"
#include "storage/localimageloader.h"
#include "storage/file_download.h"

#include <AppKit/AppKit.h>
#include <QtCore/QBuffer>
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMimeData>
#include <QtCore/QTemporaryDir>
#include <QtCore/QtEndian>
#include <cstdio>
#ifndef Q_OS_WIN
#include <sys/stat.h>
#endif

using namespace Purple;

namespace {

NSData *NativeBytes(const QByteArray &bytes) {
	return [NSData dataWithBytes:bytes.constData() length:NSUInteger(bytes.size())];
}

QByteArray Webp(QSize size, bool alpha) {
	auto image = QImage(size, alpha ? QImage::Format_ARGB32 : QImage::Format_RGB32);
	image.fill(alpha ? QColor(255, 0, 0, 80) : QColor(255, 0, 0));
	auto content = QByteArray();
	auto buffer = QBuffer(&content);
	buffer.open(QIODevice::WriteOnly);
	image.save(&buffer, "WEBP", 100);
	return content;
}

}

int main(int argc, char **argv) {
	@autoreleasepool {
		auto app = QCoreApplication(argc, argv);
		auto checks = 0;
		auto failures = 0;
		const auto check = [&](bool ok, const char *name) {
			++checks;
			if (!ok) { ++failures; std::printf("FAIL %s\n", name); }
		};
		if (argc != 3) return 2;
		auto original = QFile(QString::fromLocal8Bit(argv[1]));
		auto png = QFile(QString::fromLocal8Bit(argv[2]));
		if (!original.open(QIODevice::ReadOnly) || !png.open(QIODevice::ReadOnly)) return 2;
		const auto bytes = original.readAll();
		const auto fallback = png.readAll();
		const auto marker = QByteArray(R"({"version":1,"format":"webp","intent":"sticker","preserveAlpha":true})");
		check(ValidWebpPasteMarker(marker), "producer exact marker accepted");
		for (auto variant = 0; variant != 17; ++variant) {
			auto json = QJsonDocument::fromJson(marker).object();
			if (variant == 0) json.remove(u"version"_q);
			if (variant == 1) json.insert(u"version"_q, 2);
			if (variant == 2) json.insert(u"version"_q, u"1"_q);
			if (variant == 3) json.insert(u"version"_q, true);
			if (variant == 4) json.remove(u"format"_q);
			if (variant == 5) json.insert(u"format"_q, u"png"_q);
			if (variant == 6) json.insert(u"format"_q, 1);
			if (variant == 7) json.remove(u"intent"_q);
			if (variant == 8) json.insert(u"intent"_q, u"photo"_q);
			if (variant == 9) json.insert(u"intent"_q, false);
			if (variant == 10) json.remove(u"preserveAlpha"_q);
			if (variant == 11) json.insert(u"preserveAlpha"_q, false);
			if (variant == 12) json.insert(u"preserveAlpha"_q, u"true"_q);
			if (variant == 13) json.insert(u"preserveAlpha"_q, 1);
			if (variant == 14) json.insert(u"unexpected"_q, true);
			if (variant == 15) json.insert(u"version"_q, 0);
			if (variant == 16) json.insert(u"intent"_q, QJsonValue::Null);
			check(!ValidWebpPasteMarker(QJsonDocument(json).toJson()), "invalid marker member refuses");
		}
		check(!ValidWebpPasteMarker({}), "absent marker refuses");
		check(!ValidWebpPasteMarker("[1]"), "array marker refuses");
		check(!ValidWebpPasteMarker(marker + 'x'), "malformed marker refuses");
		check(!ValidWebpPasteMarker(QByteArray(513, ' ')), "oversize marker refuses");
		check(!ValidWebpPasteMarker(QByteArray("{\"version\":2,") + marker.sliced(1)), "duplicate marker key refuses");
		auto badUtf8 = marker;
		badUtf8.insert(1, char(0xff));
		check(!ValidWebpPasteMarker(badUtf8), "invalid UTF8 refuses");
		const auto read = ValidateMarkedWebpPaste(bytes);
		check(read && read->content == bytes && read->image.hasAlphaChannel(), "producer alpha bytes retained exactly");
		check(read && std::max(read->image.width(), read->image.height()) == 512, "producer native512 dimensions");
		check(!ValidateMarkedWebpPaste(fallback), "PNG is not raw WebP");
		check(!ValidateMarkedWebpPaste(bytes.first(11)), "short RIFF refuses");
		auto wrong = bytes;
		wrong[0] = 'X';
		check(!ValidateMarkedWebpPaste(wrong), "wrong RIFF magic refuses");
		wrong = bytes; wrong[8] = 'X';
		check(!ValidateMarkedWebpPaste(wrong), "wrong WEBP magic refuses");
		wrong = bytes; wrong[4] ^= 1;
		check(!ValidateMarkedWebpPaste(wrong), "RIFF length mismatch refuses");
		wrong = QByteArray("RIFF\4\0\0\0WEBP", 12);
		check(!ValidateMarkedWebpPaste(wrong), "undecodable RIFF refuses");
		check(!ValidateMarkedWebpPaste(bytes + QByteArray(Storage::kMaxStickerBytesSize, ' ')), "oversize bytes refuse");
		check(!ValidateMarkedWebpPaste(Webp(QSize(512, 512), false)), "opaque WebP refuses alpha intent");
		check(!ValidateMarkedWebpPaste(Webp(QSize(2561, 2561), true)), "existing sticker dimension limit enforced");
		auto temporary = QTemporaryDir();
		temporary.setAutoRemove(false);
		const auto path = QFileInfo(temporary.path()).canonicalFilePath() + u"/immutable.webp"_q;
		{
			auto file = QFile(path);
			check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "synthetic immutable file created");
		}
		const auto url = QUrl::fromLocalFile(path);
		check(ReadMarkedWebpFile(url).has_value(), "regular local WebP read");
		check(!ReadMarkedWebpFile(QUrl(u"https://example.invalid/asset.webp"_q)), "nonlocal URL refuses");
		check(!ReadMarkedWebpFile(QUrl(u"file://example.invalid/asset.webp"_q)), "remote file host refuses");
		check(!ReadMarkedWebpFile(QUrl::fromLocalFile(temporary.path())), "directory refuses");
		check(!ReadMarkedWebpFile(QUrl::fromLocalFile(path + u".missing"_q)), "missing file refuses");
#ifndef Q_OS_WIN
		const auto fifo = path + u".fifo"_q;
		check(::mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0, "synthetic FIFO created");
		check(!ReadMarkedWebpFile(QUrl::fromLocalFile(fifo)), "nonregular FIFO refuses without blocking");
#endif
		const auto link = path + u".link"_q;
		check(QFile::link(path, link), "synthetic symlink created");
		check(!ReadMarkedWebpFile(QUrl::fromLocalFile(link)), "symlink refuses");
		auto mime = QMimeData();
		mime.setData(u"application/x-purple-emoji-export"_q, marker);
		mime.setData(u"image/webp"_q, bytes);
		check(!mime.hasImage() && CanPasteMarkedWebp(&mime), "Qt raw-only marker admitted without reading a file");
		const auto qtRead = ReadMarkedWebpPaste(&mime);
		check(qtRead && qtRead->content == bytes, "Qt-produced MIME marker and bytes accepted");
		auto list = PrepareMarkedWebpPaste(&mime, 128);
		check(list && list->files.size() == 1 && list->files[0].content == bytes, "prepared upload content unchanged");
		if (list) {
			const auto &file = list->files.front();
			check(file.information->filemime == u"image/webp"_q, "existing WebP sticker MIME");
			check(file.type == Ui::PreparedFile::Type::None, "standalone sticker type excludes photo editing and albums");
			check(list->overrideSendImagesAsPhotos == false, "initial document checkbox consistent");
			for (const auto requested : { SendMediaType::Photo, SendMediaType::File }) {
				check(ClipboardWebpSendType(file, requested) == SendMediaType::File, "marked route ignores photo checkbox");
			}
			check(ClipboardWebpCompression(file, true) == false, "marked restrictions use actual sticker route");
			check(ClipboardWebpCompression(file, false) == false, "document checkbox remains document");
			check(ClipboardWebpAlbumType(file, Ui::AlbumType::PhotoVideo) == Ui::AlbumType::None, "marked sticker not in photo album");
			check(KeepClipboardWebpOriginal(file), "marked payload bypasses editor re-encoding");
		}
		auto unmarked = Ui::PreparedFile(QString());
		check(ClipboardWebpSendType(unmarked, SendMediaType::Photo) == SendMediaType::Photo, "unmarked photo route unchanged in mixed list");
		check(ClipboardWebpSendType(unmarked, SendMediaType::File) == SendMediaType::File, "unmarked document route unchanged");
		check(ClipboardWebpCompression(unmarked, std::nullopt) == std::nullopt, "unmarked unknown compression unchanged");
		check(ClipboardWebpCompression(unmarked, true) == true, "unmarked checked compression unchanged");
		check(ClipboardWebpAlbumType(unmarked, Ui::AlbumType::PhotoVideo) == Ui::AlbumType::PhotoVideo, "unmarked album unchanged");
		check(!KeepClipboardWebpOriginal(unmarked), "unmarked editor behavior unchanged");
		mime.setData(u"application/x-purple-emoji-export"_q, "bad");
		check(!CanPasteMarkedWebp(&mime), "malformed Qt marker is not admitted");
		check(!PrepareMarkedWebpPaste(&mime, 128), "malformed MIME marker takes old path");
		check(mime.data(u"image/webp"_q) == bytes, "fallthrough does not mutate mime source");
		auto ordinary = QMimeData();
		ordinary.setData(u"image/webp"_q, bytes);
		check(!CanPasteMarkedWebp(&ordinary), "unmarked admission unchanged");
		check(!PrepareMarkedWebpPaste(&ordinary, 128), "unmarked image takes old path");
		const auto board = [NSPasteboard pasteboardWithUniqueName];
		const auto write = [&](const QByteArray &mark, bool raw, bool corrupt = false) {
			[board clearContents];
			[board setData:NativeBytes(mark) forType:@"org.nightmachinery.emoji-export"];
			if (raw) [board setData:NativeBytes(corrupt ? fallback : bytes) forType:@"org.webmproject.webp"];
			[board setData:NativeBytes(url.toEncoded()) forType:@"public.file-url"];
			[board setData:NativeBytes(fallback) forType:@"public.png"];
		};
		write(marker, true);
		check([board.types containsObject:@"org.nightmachinery.emoji-export"]
			&& [board.types containsObject:@"org.webmproject.webp"]
			&& [board.types containsObject:@"public.file-url"]
			&& [board.types containsObject:@"public.png"], "producer exact four raw Cocoa types roundtrip");
		const auto native = ReadMarkedWebpPasteboard(board);
		check(native && native->content == bytes && native->image.hasAlphaChannel(), "real native reader keeps original alpha WebP");
		write(marker, false);
		const auto fromFile = ReadMarkedWebpPasteboard(board);
		check(fromFile && fromFile->content == bytes, "real native reader file URL fallback roundtrip");
		write(marker, true, true);
		check(!ReadMarkedWebpPasteboard(board), "invalid raw representation refuses rather than selecting another payload");
		write("bad", true);
		check(!ReadMarkedWebpPasteboard(board), "native malformed marker takes old path");
		[board clearContents];
		[board setData:NativeBytes(bytes) forType:@"org.webmproject.webp"];
		check(!ReadMarkedWebpPasteboard(board), "native unmarked image takes old path");
		std::printf("webp paste: %d checks, %d failures\n", checks, failures);
		return failures ? 1 : 0;
	}
}

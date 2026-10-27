#include "purple/purple_webp_paste.h"

#include "purple/purple_sync_json.h"
#include "data/data_types.h"
#include "storage/file_download.h"
#ifdef Q_OS_MAC
#include "purple/purple_webp_paste_mac.h"
#endif

#include <QtCore/QBuffer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMimeData>
#include <QtCore/QtEndian>
#include <QtGui/QImageReader>
#include <sys/stat.h>
#ifndef Q_OS_WIN
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Purple {

bool CanPasteMarkedWebp(const QMimeData *data) {
	if (!data || !data->hasFormat(u"application/x-purple-emoji-export"_q)
		|| !ValidWebpPasteMarker(data->data(u"application/x-purple-emoji-export"_q))) return false;
	return data->hasFormat(u"image/webp"_q) || data->urls().size() == 1;
}

bool ValidWebpPasteMarker(const QByteArray &marker) {
	if (marker.isEmpty() || marker.size() > 512) return false;
	const auto canonical = CanonicalizeSyncJson(marker);
	if (!canonical) return false;
	const auto document = QJsonDocument::fromJson(canonical.canonical);
	if (!document.isObject()) return false;
	const auto object = document.object();
	return object.size() == 4
		&& object.value(u"version"_q).isDouble()
		&& object.value(u"version"_q).toDouble() == 1
		&& object.value(u"format"_q).toString() == u"webp"_q
		&& object.value(u"intent"_q).toString() == u"sticker"_q
		&& object.value(u"preserveAlpha"_q).isBool()
		&& object.value(u"preserveAlpha"_q).toBool();
}

std::optional<MarkedWebpPaste> ValidateMarkedWebpPaste(QByteArray content) {
	if (content.size() < 12 || content.size() >= Storage::kMaxStickerBytesSize
		|| content.first(4) != "RIFF" || content.sliced(8, 4) != "WEBP"
		|| qFromLittleEndian<quint32>(content.constData() + 4) != content.size() - 8) {
		return std::nullopt;
	}
	auto buffer = QBuffer(&content);
	if (!buffer.open(QIODevice::ReadOnly)) return std::nullopt;
	auto reader = QImageReader(&buffer, "webp");
	const auto size = reader.size();
	if (!GoodStickerDimensions(size.width(), size.height())) return std::nullopt;
	auto image = reader.read();
	if (image.isNull() || !image.hasAlphaChannel()
		|| !GoodStickerDimensions(image.width(), image.height())) {
		return std::nullopt;
	}
	return MarkedWebpPaste{ std::move(image), std::move(content) };
}

std::optional<MarkedWebpPaste> ReadMarkedWebpFile(const QUrl &url) {
	if (!url.isValid() || !url.isLocalFile() || !url.host().isEmpty()
		|| url.hasQuery() || url.hasFragment()) return std::nullopt;
	const auto info = QFileInfo(url.toLocalFile());
	if (!info.isFile() || info.isSymLink() || info.size() <= 0
		|| info.size() >= Storage::kMaxStickerBytesSize) return std::nullopt;
	auto file = QFile(info.absoluteFilePath());
#ifdef Q_OS_WIN
	if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
	using StatInfo = struct _stat64;
	auto opened = StatInfo();
	if (::_fstat64(file.handle(), &opened) || (opened.st_mode & _S_IFMT) != _S_IFREG) {
		return std::nullopt;
	}
#else
	auto descriptor = ::open(QFile::encodeName(info.absoluteFilePath()).constData(),
		O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
	if (descriptor < 0) return std::nullopt;
	const auto close = gsl::finally([&] { if (descriptor >= 0) ::close(descriptor); });
	using StatInfo = struct stat;
	auto opened = StatInfo();
	if (::fstat(descriptor, &opened) || !S_ISREG(opened.st_mode)
		|| !file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
		return std::nullopt;
	}
	descriptor = -1;
#endif
	if (file.size() != info.size()) return std::nullopt;
	auto content = file.read(Storage::kMaxStickerBytesSize);
	if (content.size() != info.size() || !file.atEnd()) return std::nullopt;
	return ValidateMarkedWebpPaste(std::move(content));
}

std::optional<MarkedWebpPaste> ReadMarkedWebpPaste(const QMimeData *data) {
	if (!data) return std::nullopt;
	const auto marker = u"application/x-purple-emoji-export"_q;
	if (data->hasFormat(marker)) {
		if (!ValidWebpPasteMarker(data->data(marker))) return std::nullopt;
		if (data->hasFormat(u"image/webp"_q)) {
			return ValidateMarkedWebpPaste(data->data(u"image/webp"_q));
		}
		const auto urls = data->urls();
		return urls.size() == 1 ? ReadMarkedWebpFile(urls.front()) : std::nullopt;
	}
#ifdef Q_OS_MAC
	return ReadMacMarkedWebpPaste(data);
#else
	return std::nullopt;
#endif
}

}

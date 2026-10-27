#include "purple/purple_webp_paste_mac.h"

#include "storage/file_download.h"

#include <AppKit/AppKit.h>
#include <QtCore/QCoreApplication>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

namespace Purple {
namespace {

QByteArray Bytes(NSData *data) {
	return QByteArray(static_cast<const char*>(data.bytes), qsizetype(data.length));
}

}

std::optional<MarkedWebpPaste> ReadMarkedWebpPasteboard(NSPasteboard *pasteboard) {
	if (!pasteboard) return std::nullopt;
	const auto revision = pasteboard.changeCount;
	const auto items = pasteboard.pasteboardItems;
	if (items.count != 1) return std::nullopt;
	const auto item = items.firstObject;
	const auto marker = [item dataForType:@"org.nightmachinery.emoji-export"];
	if (marker.length > 512 || !ValidWebpPasteMarker(Bytes(marker))) return std::nullopt;
	auto result = std::optional<MarkedWebpPaste>();
	if ([item.types containsObject:@"org.webmproject.webp"]) {
		const auto data = [item dataForType:@"org.webmproject.webp"];
		if (data.length < Storage::kMaxStickerBytesSize) result = ValidateMarkedWebpPaste(Bytes(data));
	} else {
		const auto data = [item dataForType:@"public.file-url"];
		if (data.length <= 16384) {
			result = ReadMarkedWebpFile(QUrl::fromEncoded(Bytes(data), QUrl::StrictMode));
		}
	}
	return revision == pasteboard.changeCount ? result : std::nullopt;
}

std::optional<MarkedWebpPaste> ReadMacMarkedWebpPaste(const QMimeData *data) {
	if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())
		|| data != QGuiApplication::clipboard()->mimeData()) return std::nullopt;
	return ReadMarkedWebpPasteboard([NSPasteboard generalPasteboard]);
}

}

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QUrl>
#include <QtGui/QImage>
#include <optional>

class QMimeData;

namespace Purple {

struct MarkedWebpPaste {
	QImage image;
	QByteArray content;
};

[[nodiscard]] bool CanPasteMarkedWebp(const QMimeData *data);
[[nodiscard]] bool ValidWebpPasteMarker(const QByteArray &marker);
[[nodiscard]] std::optional<MarkedWebpPaste> ValidateMarkedWebpPaste(QByteArray content);
[[nodiscard]] std::optional<MarkedWebpPaste> ReadMarkedWebpFile(const QUrl &url);
[[nodiscard]] std::optional<MarkedWebpPaste> ReadMarkedWebpPaste(const QMimeData *data);

}

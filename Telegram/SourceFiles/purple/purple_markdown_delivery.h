#pragma once

#include "base/basic_types.h"
#include <QtCore/QString>
#include <memory>
#include <optional>

class QMimeData;

namespace Purple {

[[nodiscard]] std::unique_ptr<QMimeData> DesktopMarkdownMimeData(const std::optional<QString> &text);
[[nodiscard]] bool DeliverDesktopMarkdownText(const std::optional<QString> &text,
	const Fn<void(std::unique_ptr<QMimeData>)> &deliver);
void SetDesktopMarkdownClipboard(const QString &text);

}

#include "purple/purple_markdown_delivery.h"
#include <QtCore/QMimeData>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

namespace Purple {

std::unique_ptr<QMimeData> DesktopMarkdownMimeData(const std::optional<QString> &text) {
	if (!text) { return {}; }
	auto data = std::make_unique<QMimeData>();
	data->setText(*text);
	return data;
}

bool DeliverDesktopMarkdownText(const std::optional<QString> &text,
		const Fn<void(std::unique_ptr<QMimeData>)> &deliver) {
	auto data = DesktopMarkdownMimeData(text);
	if (!data) { return false; }
	deliver(std::move(data));
	return true;
}

void SetDesktopMarkdownClipboard(const QString &text) {
	static_cast<void>(DeliverDesktopMarkdownText(text, [](std::unique_ptr<QMimeData> data) {
		QGuiApplication::clipboard()->setMimeData(data.release());
	}));
}

}

#pragma once

#include "purple/purple_emoji_large.h"
#include <QtCore/QByteArray>
#include <QtGui/QImage>

namespace Purple {

struct EmojiLargeRender {
	QImage preview;
	QImage large;
	EmojiExportSource source = EmojiExportSource::Webp;
	int sourcePx = 0;
};

[[nodiscard]] EmojiLargeRender RenderEmojiExport(
	const QByteArray &data,
	const QString &filepath,
	EmojiExportSource source,
	bool wantLarge);

}

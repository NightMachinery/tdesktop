#include "purple/purple_emoji_large_render.h"

#include "ffmpeg/ffmpeg_frame_generator.h"
#include "ffmpeg/ffmpeg_utility.h"
#include "lottie/lottie_common.h"
#include "lottie/lottie_frame_generator.h"
#include "purple/purple_emoji_export_store.h"
#include <QtCore/QBuffer>
#include <QtCore/QFileInfo>
#include <QtGui/QImageReader>
#include <algorithm>
#include <cerrno>
#include <limits>

namespace Purple {
namespace {

constexpr auto kMaxMediaBytes = 4 * 1024 * 1024;

int ReadVideo(void *opaque, uint8_t *buffer, int maximum) {
	const auto read = static_cast<QBuffer*>(opaque)->read(
		reinterpret_cast<char*>(buffer), maximum);
	return read > 0 ? int(read) : AVERROR_EOF;
}

int64_t SeekVideo(void *opaque, int64_t offset, int whence) {
	const auto buffer = static_cast<QBuffer*>(opaque);
	if (whence == AVSEEK_SIZE) return buffer->size();
	whence &= ~AVSEEK_FORCE;
	const auto start = whence == SEEK_SET ? qint64(0)
		: whence == SEEK_CUR ? buffer->pos()
		: whence == SEEK_END ? buffer->size() : qint64(-1);
	if (start < 0 || offset > std::numeric_limits<qint64>::max() - start
		|| offset < -start) return AVERROR(EINVAL);
	const auto position = start + offset;
	return position <= buffer->size() && buffer->seek(position)
		? position : AVERROR(EINVAL);
}

QSize VideoSize(const QByteArray &bytes, bool &allowLarge) {
	auto buffer = QBuffer();
	buffer.setData(bytes);
	if (!buffer.open(QIODevice::ReadOnly)) return {};
	auto format = FFmpeg::MakeFormatPointer(&buffer, &ReadVideo, nullptr, &SeekVideo);
	if (!format || avformat_find_stream_info(format.get(), nullptr) < 0) return {};
	const auto index = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO,
		-1, -1, nullptr, 0);
	if (index < 0) return {};
	const auto stream = format->streams[index];
	const auto size = QSize(stream->codecpar->width, stream->codecpar->height);
	const auto rotation = FFmpeg::ReadRotationFromMetadata(stream);
	allowLarge = !FFmpeg::RotationSwapWidthHeight(rotation)
		|| size.width() == size.height();
	return size;
}

QSize SourceSize(const QByteArray &bytes, EmojiExportSource source, bool &allowLarge) {
	if (source == EmojiExportSource::Tgs) {
		return QSize(kEmojiLargePixels, kEmojiLargePixels);
	} else if (source == EmojiExportSource::Webm) {
		return VideoSize(bytes, allowLarge);
	}
	auto buffer = QBuffer();
	buffer.setData(bytes);
	if (!buffer.open(QIODevice::ReadOnly)) return {};
	return QImageReader(&buffer, "webp").size();
}

std::unique_ptr<Ui::FrameGenerator> Generator(
		const QByteArray &bytes, EmojiExportSource source) {
	switch (source) {
	case EmojiExportSource::Tgs: return std::make_unique<Lottie::FrameGenerator>(bytes);
	case EmojiExportSource::Webm: return std::make_unique<FFmpeg::FrameGenerator>(bytes);
	case EmojiExportSource::Webp: return std::make_unique<Ui::ImageFrameGenerator>(bytes);
	}
	return nullptr;
}

}

EmojiLargeRender RenderEmojiExport(const QByteArray &data, const QString &filepath,
		EmojiExportSource source, bool wantLarge) {
	if (data.size() > kMaxMediaBytes
		|| (data.isEmpty() && (filepath.isEmpty()
			|| QFileInfo(filepath).size() > kMaxMediaBytes))) return {};
	const auto bytes = Lottie::ReadContent(data, filepath);
	if (bytes.isEmpty() || bytes.size() > kMaxMediaBytes) return {};
	auto allowLarge = true;
	const auto native = SourceSize(bytes, source, allowLarge);
	if (native.isEmpty()) return {};
	auto generator = Generator(bytes, source);
	if (!generator) return {};
	auto result = EmojiLargeRender{ .source = source,
		.sourcePx = std::max(native.width(), native.height()) };
	result.preview = generator->renderNext(QImage(),
		QSize(kEmojiExportPixels, kEmojiExportPixels), Qt::KeepAspectRatio).image;
	const auto size = EmojiLargeRenderSize(source, native);
	if (wantLarge && allowLarge && !result.preview.isNull() && !size.isEmpty()) {
		generator->jumpToStart();
		result.large = generator->renderNext(QImage(), size, Qt::KeepAspectRatio).image;
	}
	return result;
}

}

#include "purple/purple_emoji_large_render.h"
#include "purple/purple_emoji_export_worker.h"

#include <QtCore/QBuffer>
#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtGui/QImageWriter>
#include <cstdio>
#include <zlib.h>

namespace {

auto checks = 0;
auto failures = 0;

void Check(bool value, const char *name) {
	++checks;
	if (!value) {
		++failures;
		std::printf("FAIL: %s\n", name);
	}
}

QByteArray Gzip(const QByteArray &data) {
	auto stream = z_stream{};
	if (deflateInit2(&stream, Z_BEST_SPEED, Z_DEFLATED,
		MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return {};
	auto result = QByteArray(4096, char(0));
	stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.constData()));
	stream.avail_in = data.size();
	stream.next_out = reinterpret_cast<Bytef*>(result.data());
	stream.avail_out = result.size();
	const auto code = deflate(&stream, Z_FINISH);
	result.resize(stream.total_out);
	deflateEnd(&stream);
	return code == Z_STREAM_END ? result : QByteArray();
}

void Shape(const Purple::EmojiLargeRender &result, QSize size, int px,
		Purple::EmojiExportSource source, const char *name) {
	Check(result.preview.size() == QSize(128, 128), name);
	Check(result.large.size() == size, name);
	Check(result.sourcePx == px, name);
	Check(result.source == source, name);
	if (!result.large.isNull()) {
		Check(result.large.pixelColor(size.width() / 2, size.height() / 2).alpha() > 0,
			name);
	}
}

}

int RenderCases(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	using namespace Purple;
	const auto tgs = Gzip(R"({"v":"5.5.2","fr":30,"ip":0,"op":2,"w":512,"h":512,"layers":[{"ty":4,"ip":0,"op":2,"st":0,"ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},"p":{"a":0,"k":[256,256,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},"shapes":[{"ty":"el","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[256,256]}},{"ty":"fl","c":{"a":0,"k":[1,0,0,1]},"o":{"a":0,"k":100}}]}]})");
	Check(!tgs.isEmpty(), "synthetic gzip");
	const auto vector = RenderEmojiExport(tgs, {}, EmojiExportSource::Tgs, true);
	Shape(vector, QSize(512, 512), 512, EmojiExportSource::Tgs, "TGS at 512");
	if (!vector.large.isNull()) Check(vector.large.pixelColor(0, 0).alpha() == 0,
		"TGS keeps alpha");
	auto image = QImage(QSize(100, 60), QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	image.setPixelColor(50, 30, QColor(255, 0, 0, 127));
	auto buffer = QBuffer();
	buffer.open(QIODevice::WriteOnly);
	auto writer = QImageWriter(&buffer, "webp");
	writer.setQuality(100);
	Check(writer.write(image), "synthetic WebP");
	const auto raster = RenderEmojiExport(buffer.data(), {}, EmojiExportSource::Webp, true);
	Shape(raster, QSize(100, 60), 100, EmojiExportSource::Webp, "WebP stays native");
	if (!raster.large.isNull()) {
		Check(raster.large.pixelColor(0, 0).alpha() == 0, "WebP keeps alpha");
		Check(raster.large.pixelColor(50, 30).alpha() == 127, "WebP keeps partial alpha");
	}
	const auto preview = RenderEmojiExport(buffer.data(), {}, EmojiExportSource::Webp, false);
	Check(preview.preview.size() == QSize(128, 128) && preview.large.isNull(),
		"preview-only does not render large");
	auto temporary = QTemporaryDir();
	Check(temporary.isValid(), "temporary fixture");
	auto file = QFile(temporary.filePath("source.webp"));
	Check(file.open(QIODevice::WriteOnly) && file.write(buffer.data()) == buffer.data().size(),
		"file fixture");
	file.close();
	Shape(RenderEmojiExport({}, file.fileName(), EmojiExportSource::Webp, true),
		QSize(100, 60), 100, EmojiExportSource::Webp, "file source stays native");
	Check(RenderEmojiExport({}, {}, EmojiExportSource::Webp, true).preview.isNull(),
		"missing input");
	for (const auto source : {EmojiExportSource::Tgs, EmojiExportSource::Webm,
		EmojiExportSource::Webp}) {
		Check(RenderEmojiExport("invalid", {}, source, true).preview.isNull(),
			"invalid source");
		Check(RenderEmojiExport(QByteArray(4 * 1024 * 1024 + 1, 'x'), {}, source, true)
			.preview.isNull(), "input bound");
	}
	Check(argc == 3, "WebM fixture argument");
	if (argc == 3) {
		auto video = QFile(QString::fromLocal8Bit(argv[1]));
		Check(video.open(QIODevice::ReadOnly), "WebM fixture readable");
		const auto bytes = video.readAll();
		Shape(RenderEmojiExport(bytes, {}, EmojiExportSource::Webm, true),
			QSize(100, 60), 100, EmojiExportSource::Webm, "WebM stays native");
	}
	if (argc == 3) {
		auto video = QFile(QString::fromLocal8Bit(argv[2]));
		Check(video.open(QIODevice::ReadOnly), "rotated WebM fixture readable");
		const auto result = RenderEmojiExport(video.readAll(), {}, EmojiExportSource::Webm, true);
		Check(result.preview.size() == QSize(128, 128), "unsupported native rotation retains ordinary preview");
		Check(result.large.isNull() && result.sourcePx == 100, "unsafe rotated native rectangle is omitted without upscaling");
	}
	std::printf("emoji large real render: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}

int main(int argc, char **argv) {
	QCoreApplication application(argc, argv);
	auto temporary = QTemporaryDir();
	if (!temporary.isValid()) return 2;
	const auto directory = QFileInfo(temporary.path()).canonicalFilePath() + "/emoji-export";
	auto worker = Purple::EmojiExportWorker(directory);
	auto completed = false;
	auto passed = false;
	auto beats = 0;
	auto timer = QTimer();
	QObject::connect(&timer, &QTimer::timeout, [&] { ++beats; });
	timer.start(1);
	if (!worker.submit(worker.generation(), [&](const auto &current) {
		return RenderCases(argc, argv) == 0 && current();
	}, [&](quint64, bool ok) { passed = ok; completed = true; })) return 3;
	auto elapsed = QElapsedTimer();
	elapsed.start();
	while (!completed && elapsed.elapsed() < 120000) {
		application.processEvents();
		QThread::msleep(1);
	}
	std::printf("real renderer worker: completed=%d passed=%d main heartbeat=%d\n",
		completed, passed, beats);
	return completed && passed && beats > 0 ? 0 : 1;
}

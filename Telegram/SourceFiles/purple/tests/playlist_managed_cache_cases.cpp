#include "purple/purple_playlist_managed_cache.h"
#include "purple/purple_playlist_streaming_source.h"
#include "media/streaming/media_streaming_loader_local.h"
#include "storage/cache/storage_cache_database.h"
#include "storage/storage_encryption.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <iostream>

#ifdef Q_OS_MAC
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace {

using namespace Purple;
int Failures = 0;

void Expect(bool value, const char *name) {
	if (!value) { ++Failures; std::cerr << name << '\n'; }
}

bool Wait(const Fn<bool()> &done) {
	auto timer = QElapsedTimer();
	timer.start();
	while (!done() && timer.elapsed() < 60000) {
		QCoreApplication::processEvents();
#ifdef Q_OS_MAC
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, true);
#endif
		QThread::msleep(1);
	}
	Expect(done(), "bounded real cache operation completes");
	return done();
}

bool SourceFile(const QString &path, uint64_t size) {
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly)) return false;
	auto chunk = QByteArray(int(kPlaylistStreamingPartBytes), 's');
	for (auto offset = uint64_t(0); offset != size;) {
		const auto part = offset / kPlaylistStreamingPartBytes;
		for (auto i = 0; i != 8; ++i) chunk[i] = char((part >> (8 * i)) & 255);
		const auto length = std::min<uint64_t>(chunk.size(), size - offset);
		if (file.write(chunk.constData(), length) != qint64(length)) return false;
		offset += length;
	}
	return true;
}

PlaylistDownloadStatus Transfer(Storage::Cache::Database &cache,
		std::unique_ptr<Media::Streaming::Loader> loader,
		Storage::Cache::Key key, uint64_t size) {
	struct State {
		std::optional<PlaylistDownloadStatus> result;
		std::shared_ptr<PlaylistManagedCacheTransfer> transfer;
	};
	const auto state = std::make_shared<State>();
	auto authority = std::make_shared<std::atomic<bool>>(true);
	state->transfer = std::make_shared<PlaylistManagedCacheTransfer>(PlaylistManagedCacheSource{
		.loader = std::move(loader), .key = key, .size = size, .cache = &cache,
		.current = [] { return true; },
	}, authority, [state](auto value) {
		state->result = value;
		state->transfer.reset();
	});
	state->transfer->start();
	Wait([&] { return state->result.has_value(); });
	if (state->transfer) {
		state->transfer->cancel();
		state->transfer.reset();
	}
	return state->result.value_or(PlaylistDownloadStatus::IoError);
}

} // namespace

int main(int argc, char *argv[]) {
	QCoreApplication app(argc, argv);
	Expect(!PlaylistStreamingSizeSupported(0)
		&& PlaylistStreamingSizeSupported(4278190080ULL)
		&& !PlaylistStreamingSizeSupported(4278190081ULL)
		&& !PlaylistStreamingSizeSupported(4294967295ULL),
		"admission respects native slice count and overflow rather than uint32 alone");
	QTemporaryDir root;
	Expect(root.isValid(), "private temporary directory is valid before fixture I/O");
	if (!root.isValid()) return 1;
	const auto file = root.path() + u"/source.bin"_q;
	const auto size = uint64_t(36 * 1024 * 1024 + 17);
	const auto sourceCreated = SourceFile(file, size);
	Expect(sourceCreated, "distinct real parts create a source larger than 10 MiB");
	if (!sourceCreated) return 1;
	auto settings = Storage::Cache::Database::Settings();
	settings.totalSizeLimit = 256 * 1024 * 1024;
	Storage::Cache::Database database(root.path() + u"/native-cache"_q, settings);
	struct OpenState {
		std::atomic<bool> done = false;
		std::atomic<Storage::Cache::Error::Type> error = Storage::Cache::Error::Type::IO;
	};
	const auto opened = std::make_shared<OpenState>();
	const auto keyBytes = QByteArray(Storage::EncryptionKey::kSize, 'q');
	database.open(Storage::EncryptionKey(bytes::make_vector(bytes::make_span(keyBytes))), [opened](auto value) {
		opened->error.store(value.type, std::memory_order_release);
		opened->done.store(true, std::memory_order_release);
	});
	const auto closeCache = [&] {
		const auto closed = std::make_shared<std::atomic<bool>>(false);
		database.close([closed] { closed->store(true, std::memory_order_release); });
		return Wait([&] { return closed->load(std::memory_order_acquire); });
	};
	if (!Wait([&] { return opened->done.load(std::memory_order_acquire); })
		|| opened->error.load(std::memory_order_acquire) != Storage::Cache::Error::Type::None) {
		closeCache();
		return 1;
	}
	const auto key = Storage::Cache::Key{ 123, 1024 };
	auto local = BindPlaylistStreamingSource(Media::Streaming::MakeFileLoader(file), key);
	Expect(bool(local), "genuine local source is bound to a native cache key");
	if (!local) {
		closeCache();
		return 1;
	}
	local->clearAttachedDownloader();
	Expect(Transfer(database, std::move(local), key, size) == PlaylistDownloadStatus::Ready,
		"large song writes and independently verifies every cache byte");
	Expect(QFile::remove(file), "only disposable original source is removed");
	Expect(Transfer(database, MakePlaylistCacheOnlySource(key, size), key, size) == PlaylistDownloadStatus::Ready,
		"complete cache survives source deletion without any source/network fallback");
	const auto removed = std::make_shared<std::atomic<bool>>(false);
	database.remove({ key.high, key.low + 3 }, [removed](auto) {
		removed->store(true, std::memory_order_release);
	});
	if (!Wait([&] { return removed->load(std::memory_order_acquire); })) {
		closeCache();
		return 1;
	}
	Expect(Transfer(database, MakePlaylistCacheOnlySource(key, size), key, size) != PlaylistDownloadStatus::Ready,
		"missing late slice cannot pass from header metadata or enqueue counts");
	Expect(SourceFile(file, size), "source is recreated for closed-cache failure");
	if (!closeCache()) return 1;
	Expect(Transfer(database,
		BindPlaylistStreamingSource(Media::Streaming::MakeFileLoader(file), { 123, 2048 }),
		{ 123, 2048 }, size) != PlaylistDownloadStatus::Ready,
		"all source bytes delivered to a closed cache never count as downloaded");
	auto called = false;
	auto authority = std::make_shared<std::atomic<bool>>(true);
	auto cancelled = std::make_shared<PlaylistManagedCacheTransfer>(PlaylistManagedCacheSource{
		.loader = BindPlaylistStreamingSource(Media::Streaming::MakeFileLoader(file), key),
		.key = key, .size = size, .cache = &database, .current = [] { return true; },
	}, authority, [&](auto) { called = true; });
	cancelled->start();
	cancelled->cancel();
	cancelled.reset();
	QCoreApplication::processEvents();
	Expect(!called && !authority->load(std::memory_order_acquire),
		"immediate retirement revokes delivery while pending cache callbacks are harmless");
	return Failures ? 1 : 0;
}

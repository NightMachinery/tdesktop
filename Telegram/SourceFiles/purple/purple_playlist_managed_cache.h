#pragma once

#include "base/basic_types.h"
#include "purple/purple_playlist_download_state.h"
#include "data/data_file_origin.h"
#include "storage/cache/storage_cache_types.h"
#include "media/streaming/media_streaming_loader.h"
#include <QtCore/QCryptographicHash>
#include <atomic>
#include <thread>

class DocumentData;
namespace Media::Streaming { class Loader; class Reader; }
namespace Storage::Cache { class Database; }

namespace Purple {

struct PlaylistManagedCacheSource {
	std::unique_ptr<Media::Streaming::Loader> loader;
	Storage::Cache::Key key;
	uint64_t size = 0;
	Storage::Cache::Database *cache = nullptr;
	Fn<bool()> current;
};

class PlaylistManagedCacheTransfer final
	: public std::enable_shared_from_this<PlaylistManagedCacheTransfer> {
public:
	PlaylistManagedCacheTransfer(
		not_null<DocumentData*> document, Data::FileOrigin origin,
		std::shared_ptr<std::atomic<bool>> authority,
		Fn<void(PlaylistDownloadStatus)> done);
	PlaylistManagedCacheTransfer(
		PlaylistManagedCacheSource source,
		std::shared_ptr<std::atomic<bool>> authority,
		Fn<void(PlaylistDownloadStatus)> done);
	~PlaylistManagedCacheTransfer();
	void start();
	void cancel();

private:
	struct Signals;
	enum class Phase { Write, Readback, Presence };
	[[nodiscard]] bool current() const;
	void wake();
	void pump();
	void barrier(Fn<void()> resume = nullptr);
	void passDone();
	void beginCachePass(Phase phase);
	void finish(PlaylistDownloadStatus status);
	void stopRelay();
	void releaseReader();
	void releaseSlot(Fn<void()> ready = nullptr);

	Fn<bool()> _current;
	Storage::Cache::Key _key;
	uint64_t _size = 0;
	uint64_t _offset = 0;
	std::shared_ptr<std::atomic<bool>> _authority;
	Fn<void(PlaylistDownloadStatus)> _done;
	std::shared_ptr<void> _slot;
	Storage::Cache::Database *_cache = nullptr;
	std::unique_ptr<Media::Streaming::Loader> _source;
	std::shared_ptr<Media::Streaming::Reader> _reader;
	std::shared_ptr<Signals> _signals;
	std::thread _relay;
	QByteArray _buffer;
	QCryptographicHash _hash = QCryptographicHash(QCryptographicHash::Sha256);
	QByteArray _expectedHash;
	QByteArray _prefixHash;
	Phase _phase = Phase::Write;
	bool _writesDrained = false;
	bool _headerDone = false;
	bool _barrierPending = false;
	bool _started = false;
	bool _terminal = false;
	bool _cancelled = false;
	PlaylistDownloadStatus _admission = PlaylistDownloadStatus::Changed;

};

} // namespace Purple

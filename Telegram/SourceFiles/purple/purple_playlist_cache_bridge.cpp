#include "purple/purple_playlist_cache_bridge.h"

#include "media/streaming/media_streaming_reader.h"
#include <mutex>
#include <set>

namespace Purple {
namespace {

std::mutex ReadersMutex;
std::set<const Media::Streaming::Reader*> Readers;

} // namespace

void PlaylistCacheBridge::Adopt(not_null<Media::Streaming::Reader*> reader) {
	const auto lock = std::lock_guard(ReadersMutex);
	Readers.insert(reader.get());
}

bool PlaylistCacheBridge::Flush(not_null<Media::Streaming::Reader*> reader) {
	{
		const auto lock = std::lock_guard(ReadersMutex);
		if (!Readers.contains(reader.get())) return false;
	}
	if (!reader->_cacheHelper || !reader->_cache) return false;
	auto slice = reader->_slices.unloadToCache();
	while (slice.number >= 0) {
		reader->putToCache(std::move(slice));
		slice = reader->_slices.unloadToCache();
	}
	return true;
}

bool PlaylistCacheBridge::SkipFinalize(const Media::Streaming::Reader *reader) {
	const auto lock = std::lock_guard(ReadersMutex);
	return Readers.erase(reader) != 0;
}

} // namespace Purple

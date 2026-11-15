#pragma once

#include "data/data_file_origin.h"
#include "media/streaming/media_streaming_loader.h"

class DocumentData;

namespace Purple {

inline constexpr auto kPlaylistStreamingPartBytes = uint64_t(Media::Streaming::Loader::kPartSize);
inline constexpr auto kPlaylistStreamingSliceBytes = 64 * kPlaylistStreamingPartBytes;
inline constexpr auto kPlaylistStreamingMaximumBytes = 510 * kPlaylistStreamingSliceBytes;

[[nodiscard]] bool PlaylistStreamingSizeSupported(uint64_t size);
[[nodiscard]] std::unique_ptr<Media::Streaming::Loader> MakePlaylistStreamingSource(
	not_null<DocumentData*> document, Data::FileOrigin origin, bool managedCache);
[[nodiscard]] std::unique_ptr<Media::Streaming::Loader> BindPlaylistStreamingSource(
	std::unique_ptr<Media::Streaming::Loader> loader,
	Storage::Cache::Key key);
[[nodiscard]] std::unique_ptr<Media::Streaming::Loader> MakePlaylistCacheOnlySource(
	Storage::Cache::Key key, uint64_t size);

} // namespace Purple

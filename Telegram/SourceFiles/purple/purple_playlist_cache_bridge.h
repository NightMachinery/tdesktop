#pragma once

#include "base/basic_types.h"

namespace Media::Streaming { class Reader; }

namespace Purple {

class PlaylistCacheBridge final {
public:
	static void Adopt(not_null<Media::Streaming::Reader*> reader);
	[[nodiscard]] static bool Flush(not_null<Media::Streaming::Reader*> reader);
	[[nodiscard]] static bool SkipFinalize(const Media::Streaming::Reader *reader);

};

} // namespace Purple

#pragma once

#include "purple/purple_playlists_resolve_queue.h"

namespace Main { class Session; }

namespace Purple {

class DesktopPlaylistResolver final {
public:
	DesktopPlaylistResolver(
		not_null<Main::Session*> session,
		std::vector<PlaylistResolveSong> songs,
		Fn<void(std::vector<PlaylistSongResolution>)> done);
	~DesktopPlaylistResolver();
	void start();

private:
	class Impl;
	std::unique_ptr<Impl> _impl;

};

} // namespace Purple

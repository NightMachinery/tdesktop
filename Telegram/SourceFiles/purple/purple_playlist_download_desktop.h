#pragma once

#include "purple/purple_playlist_download_job.h"

namespace Main { class Session; }

namespace Purple {

class DesktopPlaylistDownloadJob final {
public:
	DesktopPlaylistDownloadJob(
		not_null<Main::Session*> session, QString playlistId,
		Fn<void(PlaylistDownloadProgress)> changed);
	~DesktopPlaylistDownloadJob();
	void start();
	void retry();
	void cancel();

private:
	class State;
	std::shared_ptr<State> _state;

};

} // namespace Purple

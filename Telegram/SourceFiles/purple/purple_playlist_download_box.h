#pragma once

namespace Window { class SessionController; }

namespace Purple {

void ShowPlaylistDownloadsBox(
	not_null<Window::SessionController*> controller,
	QString playlistId,
	Fn<void()> closed);

} // namespace Purple

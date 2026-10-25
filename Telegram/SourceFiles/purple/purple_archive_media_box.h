#pragma once

#include "purple/purple_archive_media_access.h"

namespace Window {
class SessionController;
}

namespace Purple {

void ShowArchiveMedia(not_null<Window::SessionController*> controller,
	ArchiveMediaRequest request, QString mime, bool photo,
	std::function<bool()> originCurrent);

}

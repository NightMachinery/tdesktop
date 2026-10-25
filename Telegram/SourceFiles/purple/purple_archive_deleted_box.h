#pragma once

#include "purple/purple_archive_views.h"

namespace Window {
class SessionController;
}

namespace Purple {

void ShowArchiveDeleted(not_null<Window::SessionController*> controller,
	ArchiveViewScope scope, QString name, std::function<bool()> scopeCurrent);

}

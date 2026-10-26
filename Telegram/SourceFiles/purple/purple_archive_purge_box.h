#pragma once

#include "purple/purple_archive_views.h"

namespace Window {
class SessionController;
}

namespace Purple {

void ShowArchivePurgeBox(not_null<Window::SessionController *> controller,
	ArchivePurgeKind kind,
	ArchivePurgeScope scope,
	QString scopeName,
	std::function<bool()> scopeCurrent);

}

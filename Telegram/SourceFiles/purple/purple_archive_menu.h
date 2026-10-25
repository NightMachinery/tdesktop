#pragma once

#include "window/window_peer_menu.h"
#include "purple/purple_archive_views.h"

namespace Purple {

[[nodiscard]] ArchiveViewScope ArchiveDeletedScope(not_null<PeerData*> peer,
	std::optional<int64> topic = {});
[[nodiscard]] bool SameArchiveViewScope(const ArchiveViewScope &a, const ArchiveViewScope &b);
void AddArchiveChatActions(not_null<Window::SessionController*> controller,
	Dialogs::EntryState request, const Window::PeerMenuCallback &callback);

}

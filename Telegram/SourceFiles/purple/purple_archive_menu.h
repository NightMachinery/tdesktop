#pragma once

#include "window/window_peer_menu.h"

namespace Purple {
void AddArchiveChatActions(not_null<Window::SessionController*> controller,
	Dialogs::EntryState request, const Window::PeerMenuCallback &callback);
}

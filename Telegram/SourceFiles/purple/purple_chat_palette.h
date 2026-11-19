#pragma once

class PeerListController;

namespace Window { class SessionController; }

namespace Purple {

[[nodiscard]] bool IsChatPaletteController(const PeerListController *controller);
void ShowChatPalette(not_null<Window::SessionController*> controller);

} // namespace Purple

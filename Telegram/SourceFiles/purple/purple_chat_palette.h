#pragma once

class PeerListController;
class PeerData;

namespace Window { class SessionController; }

namespace Purple {

[[nodiscard]] bool IsChatPaletteController(const PeerListController *controller);
void RecordChatPaletteVisit(not_null<PeerData*> peer);
void ShowChatPalette(not_null<Window::SessionController*> controller, bool recent);

} // namespace Purple

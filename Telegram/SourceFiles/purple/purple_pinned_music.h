#pragma once

namespace Data {
class Thread;
}

namespace Window {
class SessionController;
}

namespace Purple {

void ShowPinnedMusicBox(
	not_null<Window::SessionController*> controller,
	not_null<Data::Thread*> thread);

} // namespace Purple

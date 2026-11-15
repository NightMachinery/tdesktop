#pragma once

#include "purple/purple_playlists_menu.h"

namespace Ui { class VerticalLayout; }

namespace Purple {

void AddPlaylistsMenuRow(
	not_null<Ui::VerticalLayout*> menu,
	not_null<Window::SessionController*> controller);
void ShowPlaylistsBox(not_null<Window::SessionController*> controller);
void ShowPlaylistPicker(
	not_null<Window::SessionController*> controller,
	PlaylistMessage message);

} // namespace Purple

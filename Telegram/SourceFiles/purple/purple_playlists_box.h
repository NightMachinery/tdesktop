#pragma once

#include "purple/purple_playlists_menu.h"

#include <optional>

namespace Ui { class VerticalLayout; }

namespace Purple {

void AddPlaylistsMenuRow(
	not_null<Ui::VerticalLayout*> menu,
	not_null<Window::SessionController*> controller);
void ShowPlaylistsBox(not_null<Window::SessionController*> controller);
void ShowPlaylistBox(
	not_null<Window::SessionController*> controller,
	const QString &playlistId,
	std::optional<uint64_t> focusedDocumentId = std::nullopt);
void ShowPlaylistPicker(
	not_null<Window::SessionController*> controller,
	PlaylistMessage message);
void ShowPlaylistPicker(
	not_null<Window::SessionController*> controller,
	std::vector<PlaylistMessage> messages);

} // namespace Purple

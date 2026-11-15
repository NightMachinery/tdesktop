#include "purple/purple_playlist_player_ui.h"

#include "base/weak_ptr.h"
#include "main/main_session.h"
#include "media/player/media_player_instance.h"
#include "purple/purple_playlist_player.h"
#include "purple/purple_playlists_box.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

namespace Purple {
namespace {

std::optional<PlaylistPlayerUiTarget> Target(
		not_null<Window::SessionController*> controller,
		AudioMsgId::Type type) {
	if (type != AudioMsgId::Type::Song
		|| controller->window().sessionController() != controller.get()) {
		return std::nullopt;
	}
	const auto weak = base::make_weak(controller.get());
	const auto session = base::make_weak(&controller->session());
	const auto target = CurrentAccountPlaylistTarget(
		session.get(),
		Media::Player::instance()->current(type));
	const auto alive = weak.get();
	if (!alive || !session || &alive->session() != session.get()
		|| alive->window().sessionController() != alive) {
		return std::nullopt;
	}
	return target;
}

} // namespace

bool AccountPlaylistTrackClickable(
		not_null<Window::SessionController*> controller,
		AudioMsgId::Type type) {
	return Target(controller, type).has_value();
}

bool OpenAccountPlaylistFromPlayer(
		not_null<Window::SessionController*> controller,
		AudioMsgId::Type type) {
	const auto target = Target(controller, type);
	if (!target) return false;
	ShowPlaylistBox(controller, target->playlistId, target->documentId);
	return true;
}

} // namespace Purple

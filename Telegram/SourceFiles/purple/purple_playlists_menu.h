#pragma once

#include "data/data_msg_id.h"
#include "purple/purple_playlists_ops.h"
#include "ui/widgets/menu/menu_add_action_callback.h"

namespace Main { class Session; }
namespace Window { class SessionController; }
class DocumentData;
class HistoryItem;

namespace Purple {

struct PlaylistMessage {
	FullMsgId id;
	uint64_t documentId = 0;
};

[[nodiscard]] std::optional<PlaylistAddSongInput> CapturePlaylistSong(
	not_null<Main::Session*> session,
	const PlaylistMessage &message);
void AddPlaylistActions(
	const Ui::Menu::MenuCallback &add,
	not_null<HistoryItem*> item,
	not_null<DocumentData*> document,
	not_null<Window::SessionController*> controller);

} // namespace Purple

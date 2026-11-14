#pragma once

#include "purple/purple_playlists_worker.h"

namespace Main { class Account; class Session; }

namespace Purple {

using PlaylistAccountDone = Fn<void(PlaylistAccountResult)>;

void ReadAccountPlaylists(
	not_null<Main::Session*> session,
	PlaylistAccountDone done);
void EditAccountPlaylists(
	not_null<Main::Session*> session,
	PlaylistAccountEdit request,
	PlaylistAccountDone done);
void InvalidatePlaylistServices();
void StopAccountPlaylists(Main::Account &account);

} // namespace Purple

#include "purple/purple_playlists_menu.h"

#include "base/weak_ptr.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "purple/purple_playlists_box.h"
#include "purple/purple_playlists_policy.h"
#include "purple/purple_user_control.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include "styles/style_menu_icons.h"

namespace Purple {

std::optional<PlaylistAddSongInput> CapturePlaylistSong(
		not_null<Main::Session*> session,
		const PlaylistMessage &message) {
	if (session->account().loggingOut()
		|| session->account().maybeSession() != session) {
		return std::nullopt;
	}
	const auto item = session->data().message(message.id);
	if (!item || !item->computeUnavailableReason().isEmpty()) {
		return std::nullopt;
	}
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	if (!document || document->id != message.documentId
		|| item->id.bare <= 0 || item->id.bare > kMaximumPlaylistMessageId) {
		return std::nullopt;
	}
	const auto candidate = PlaylistSongCandidate{
		.documentId = document->id,
		.messageId = int32_t(item->id.bare),
		.hasAudioAttribute = document->isSong(),
		.isVoiceMessage = document->isVoiceMessage(),
		.isRegularServerMessage = item->isRegular(),
		.isMainMessageMedia = dynamic_cast<Data::MediaFile*>(media) != nullptr,
		.isScheduled = item->isScheduled(),
		.isLocalUnsent = item->isLocal() || item->isSending(),
		.isSponsored = item->isSponsored(),
		.hasSelfDestructTimer = bool(media->ttlSeconds()) || bool(item->ttlDestroyAt()),
		.isProtectedContent = !item->allowsForward(),
	};
	if (!QualifySong(candidate, SaveRestrictionsDisabled())) {
		return std::nullopt;
	}
	const auto song = document->song();
	const auto meta = PrepareSongMeta({
		.title = song ? song->title : QString(),
		.performer = song ? song->performer : QString(),
		.fileName = document->filename(),
		.durationSeconds = document->duration() / 1000,
	});
	if (!meta) {
		return std::nullopt;
	}
	const auto peer = item->history()->peer;
	const auto token = peer->isSelf()
		? PeerToken{ PeerTokenKind::Self, 0 }
		: peerIsUser(peer->id)
		? PeerToken{ PeerTokenKind::User, peerToUser(peer->id).bare }
		: peerIsChat(peer->id)
		? PeerToken{ PeerTokenKind::Chat, peerToChat(peer->id).bare }
		: PeerToken{ PeerTokenKind::Channel, peerToChannel(peer->id).bare };
	return PlaylistAddSongInput{
		.documentId = document->id,
		.peer = token,
		.msg = int32_t(item->id.bare),
		.username = peer->isChannel() ? peer->username() : QString(),
		.meta = meta.meta,
	};
}

void AddPlaylistActions(
		const Ui::Menu::MenuCallback &add,
		not_null<HistoryItem*> item,
		not_null<DocumentData*> document,
		not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	const auto message = PlaylistMessage{ item->fullId(), document->id };
	if (!CapturePlaylistSong(session, message)) {
		return;
	}
	const auto window = base::make_weak(controller.get());
	const auto weakSession = base::make_weak(session);
	const auto account = SessionAccountKey(session);
	add(tr::lng_purple_playlists_add(tr::now), [=] {
		const auto own = window.get();
		const auto live = weakSession.get();
		if (own && live && &own->session() == live
			&& own->window().sessionController() == own
			&& live->account().maybeSession() == live
			&& !live->account().loggingOut()
			&& SessionAccountKey(live) == account
			&& CapturePlaylistSong(live, message)) {
			ShowPlaylistPicker(own, message);
		}
	}, &st::menuIconFile);
}

} // namespace Purple

#pragma once

#include "purple/purple_playlists_store.h"
#include "purple/purple_playlists_ops.h"

#include <atomic>

namespace Purple {

enum class PlaylistAccountEditKind {
	Create,
	Copy,
	Rename,
	Delete,
	Restore,
	DeleteForever,
	AddSongs,
	RemoveSongs,
	ReorderSongs,
	Undo,
};

struct PlaylistAccountEdit {
	PlaylistAccountEditKind kind = PlaylistAccountEditKind::Create;
	QString playlistId;
	QString title;
	QByteArray entropy;
	Playlist source;
	std::vector<PlaylistAddSongInput> songs;
	std::vector<uint64_t> documentIds;
	PlaylistUndoRecord undo;
	uint64_t nowMs = 0;
};

struct PlaylistAccountResult {
	PlaylistStoreRead snapshot;
	std::optional<PlaylistEditResult> edit;
	std::optional<PlaylistStoreWrite> write;

	[[nodiscard]] explicit operator bool() const {
		return bool(snapshot) && (!edit || bool(*edit)) && (!write || bool(*write));
	}
};

class PlaylistAccountWorker final {
public:
	PlaylistAccountWorker(
		PlaylistStoreRoute route,
		std::shared_ptr<std::atomic<bool>> authority);
	[[nodiscard]] PlaylistAccountResult read();
	[[nodiscard]] PlaylistAccountResult edit(const PlaylistAccountEdit &request);

private:
	[[nodiscard]] PlaylistEditResult apply(
		const PlaylistLibrary &library,
		const PlaylistAccountEdit &request) const;

	uint64_t _selfUserId = 0;
	std::shared_ptr<std::atomic<bool>> _authority;
	DesktopPlaylistStore _store;
	bool _opened = false;

};

} // namespace Purple

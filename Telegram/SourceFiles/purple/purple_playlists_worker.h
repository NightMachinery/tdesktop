#pragma once

#include "purple/purple_playlists_store.h"
#include "purple/purple_playlist_download_state.h"
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

struct PlaylistAccountUse {
	QString playlistId;
	uint64_t nowMs = 0;
};

enum class PlaylistDownloadRequestKind { Read, Preference, Register, RemoveNow, Prune, Verify, PrepareTransfer, CommitTransfer, DiscardTransfer };

struct PlaylistAccountDownloadRequest {
	PlaylistDownloadRequestKind kind = PlaylistDownloadRequestKind::Read;
	QString playlistId;
	QString transferToken;
	PlaylistKeepDownloaded preference = PlaylistKeepDownloaded::Inherit;
	uint64_t documentId = 0;
	PlaylistSource expectedSource;
	PlaylistKeptFile file;
	std::optional<PlaylistKeptFile> replacesMissing;
	std::weak_ptr<std::atomic<bool>> jobAuthority;
};

struct PlaylistAccountResult {
	PlaylistStoreRead snapshot;
	std::optional<PlaylistEditResult> edit;
	std::optional<PlaylistStoreWrite> write;
	std::optional<PlaylistDownloadRead> downloads;

	[[nodiscard]] explicit operator bool() const {
		return bool(snapshot) && (!edit || bool(*edit)) && (!write || bool(*write))
			&& (!downloads || bool(*downloads));
	}
};

class PlaylistAccountWorker final {
public:
	PlaylistAccountWorker(
		PlaylistStoreRoute route,
		std::shared_ptr<std::atomic<bool>> authority);
	[[nodiscard]] PlaylistAccountResult read();
	[[nodiscard]] PlaylistAccountResult edit(const PlaylistAccountEdit &request);

	[[nodiscard]] PlaylistAccountResult recordUse(
		const PlaylistAccountUse &request);

	[[nodiscard]] PlaylistAccountResult download(
		const PlaylistAccountDownloadRequest &request);

private:
	[[nodiscard]] PlaylistAccountResult transfer(
		const PlaylistAccountDownloadRequest &request);
	[[nodiscard]] PlaylistEditResult apply(
		const PlaylistLibrary &library,
		const PlaylistAccountEdit &request) const;

	uint64_t _selfUserId = 0;
	std::shared_ptr<std::atomic<bool>> _authority;
	DesktopPlaylistStore _store;
	bool _opened = false;
	std::map<QString, std::weak_ptr<std::atomic<bool>>> _downloadOwners;

};

} // namespace Purple

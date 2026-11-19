#pragma once

#include "purple/purple_playlists_local.h"
#include "purple/purple_playlists_ops.h"

namespace Purple {

struct PlaylistFolders {
	std::map<QString, QString> names;
	std::map<QString, QString> placements;

	[[nodiscard]] bool operator==(const PlaylistFolders &) const = default;
};

[[nodiscard]] QString HyperFolderId();

[[nodiscard]] std::optional<PlaylistFolders> ReadPlaylistFolders(
	const PlaylistLocalState &state);
[[nodiscard]] bool WritePlaylistFolders(
	PlaylistLocalState &state,
	const PlaylistFolders &folders);

[[nodiscard]] bool SetPlaylistFolderName(
	PlaylistFolders &folders,
	const QString &id,
	const QString &name);
[[nodiscard]] bool RemovePlaylistFolder(
	PlaylistFolders &folders,
	const QString &id);
[[nodiscard]] bool MovePlaylistToFolder(
	PlaylistFolders &folders,
	const PlaylistLibrary &library,
	const QString &playlistId,
	const QString &folderId);

struct HyperPlaylist {
	PlaylistEditResult edit;
	PlaylistFolders folders;
};

[[nodiscard]] std::optional<HyperPlaylist> PrepareHyperPlaylist(
	const PlaylistLibrary &library,
	const PlaylistFolders &folders,
	const std::vector<PlaylistAddSongInput> &songs,
	const QByteArray &entropy,
	uint64_t nowMs,
	uint64_t selfUserId);

} // namespace Purple

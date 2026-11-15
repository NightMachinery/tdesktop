#pragma once

#include "purple/purple_playlist_download_state.h"

namespace Purple {

struct PlaylistKeptFileRead {
	PlaylistDownloadStatus status = PlaylistDownloadStatus::Invalid;
	PlaylistKeptFile file;
	[[nodiscard]] explicit operator bool() const {
		return status == PlaylistDownloadStatus::Ready;
	}
};

[[nodiscard]] QString PlaylistStagingFilePath(
	const QString &directory, const QString &token);
[[nodiscard]] PlaylistKeptFileRead CreatePlaylistStagingFile(
	const QString &directory, const QString &token);
[[nodiscard]] PlaylistKeptFileRead InspectPlaylistStagingFile(
	const QString &directory, const QString &token,
	const PlaylistOwnedDownloadIntent &intent);
[[nodiscard]] PlaylistDownloadStatus DiscardPlaylistStagingFile(
	const QString &directory, const QString &token, const QByteArray &identity);
[[nodiscard]] PlaylistKeptFileRead PublishPlaylistStagingFile(
	const QString &directory, const QString &token,
	const PlaylistOwnedDownloadIntent &intent);
[[nodiscard]] QString PlaylistKeptFilePath(
	const QString &directory, uint64_t id, const QString &extension);
[[nodiscard]] PlaylistKeptFileRead InspectPlaylistKeptFile(
	const QString &directory, uint64_t id,
	const QString &extension, uint64_t expectedSize);
[[nodiscard]] PlaylistDownloadStatus RemovePlaylistKeptFile(
	const QString &directory, uint64_t id, const PlaylistKeptFile &expected);

} // namespace Purple

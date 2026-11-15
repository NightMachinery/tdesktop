#pragma once

#include "purple/purple_playlist_download_state.h"

#include <functional>

namespace Purple {

using PlaylistFileAuthority = std::function<bool()>;

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
	const PlaylistOwnedDownloadIntent &intent,
	const PlaylistFileAuthority &authority = {});
[[nodiscard]] PlaylistDownloadStatus DiscardPlaylistStagingFile(
	const QString &directory, const QString &token, const QByteArray &identity);
[[nodiscard]] PlaylistKeptFileRead PublishPlaylistStagingFile(
	const QString &directory, const QString &token,
	const PlaylistOwnedDownloadIntent &intent,
	const PlaylistFileAuthority &authority = {});
[[nodiscard]] QString PlaylistKeptFilePath(
	const QString &directory, uint64_t id, const QString &extension);
[[nodiscard]] PlaylistKeptFileRead InspectPlaylistKeptFile(
	const QString &directory, uint64_t id,
	const QString &extension, uint64_t expectedSize,
	const PlaylistFileAuthority &authority = {});
[[nodiscard]] PlaylistDownloadStatus RemovePlaylistKeptFile(
	const QString &directory, uint64_t id, const PlaylistKeptFile &expected,
	const PlaylistFileAuthority &authority = {});

} // namespace Purple

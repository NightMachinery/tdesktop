#pragma once

#include "purple/purple_playlists_local.h"

namespace Purple {

enum class PlaylistKeepDownloaded { Inherit, On, Off };
enum class PlaylistDownloadStatus {
	Ready, Invalid, Newer, Changed, Busy, IoError, Unsupported,
};

struct PlaylistKeptFile {
	QString extension;
	uint64_t size = 0;
	QByteArray identity;
	QByteArray sha256;
	QJsonObject preserved;
	[[nodiscard]] bool operator==(const PlaylistKeptFile &) const = default;
};

struct PlaylistOwnedDownloadIntent {
	QString playlistId;
	uint64_t documentId = 0;
	QString extension;
	uint64_t size = 0;
	QByteArray identity;
	QByteArray sha256;
	QJsonObject preserved;
	[[nodiscard]] bool operator==(const PlaylistOwnedDownloadIntent &) const = default;
};

struct PlaylistDownloadPrepared {
	QString token;
	PlaylistOwnedDownloadIntent intent;
	bool complete = false;
	bool alreadyKept = false;
};

struct PlaylistDownloadState {
	std::map<QString, PlaylistKeepDownloaded> preferences;
	std::map<uint64_t, PlaylistKeptFile> files;
	std::map<QString, PlaylistOwnedDownloadIntent> pending;
	QJsonObject preserved;
	[[nodiscard]] bool operator==(const PlaylistDownloadState &) const = default;
};

struct PlaylistDownloadRead {
	PlaylistDownloadStatus status = PlaylistDownloadStatus::Invalid;
	PlaylistDownloadState state;
	std::optional<PlaylistDownloadPrepared> prepared;
	std::set<uint64_t> verified;
	[[nodiscard]] explicit operator bool() const {
		return status == PlaylistDownloadStatus::Ready;
	}
};

[[nodiscard]] bool PlaylistDownloadTokenSafe(const QString &token);
[[nodiscard]] bool PlaylistDownloadExtensionSafe(const QString &extension);
[[nodiscard]] PlaylistDownloadRead ReadPlaylistDownloadState(
	const PlaylistLocalState &state);
[[nodiscard]] PlaylistDownloadStatus SetPlaylistDownloadState(
	PlaylistLocalState &local, const PlaylistDownloadState &state);
[[nodiscard]] bool PlaylistKeepsDownloads(
	const PlaylistDownloadState &state, const QString &playlistId,
	bool appWideDefault);
[[nodiscard]] std::set<uint64_t> PlaylistDownloadReferences(
	const PlaylistLibrary &library, const QString &excluding = {});
[[nodiscard]] std::vector<uint64_t> PlaylistDownloadRemovalCandidates(
	const PlaylistLibrary &library, const PlaylistDownloadState &state,
	const std::optional<QString> &removeFromPlaylist);

} // namespace Purple

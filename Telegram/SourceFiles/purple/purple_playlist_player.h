#pragma once

#include "data/data_audio_msg_id.h"
#include "purple/purple_playlists_resolve_queue.h"

#include <rpl/producer.h>

#include <cstddef>
#include <optional>
#include <vector>

namespace Main { class Session; }

namespace Purple {

struct PlaylistMessage;

enum class PlaylistPlaybackPhase {
	Inactive,
	Loading,
	Resolving,
	Playing,
	RetryRequired,
	Exhausted,
	Retired,
};

enum class PlaylistPlaybackProblem {
	None,
	LibraryUnavailable,
	PlaylistUnavailable,
	SelectedSongMissing,
	SourceUnavailable,
	TransientFailure,
	PlaybackRejected,
	UnsupportedMode,
};

struct PlaylistPlaybackStatus {
	QString playlistId;
	uint64_t documentId = 0;
	std::optional<std::size_t> index;
	std::size_t total = 0;
	PlaylistPlaybackPhase phase = PlaylistPlaybackPhase::Inactive;
	PlaylistPlaybackProblem problem = PlaylistPlaybackProblem::None;
	std::optional<PlaylistSongStatus> sourceStatus;
};

struct PlaylistPlayerUiTarget {
	QString playlistId;
	uint64_t documentId = 0;
};

[[nodiscard]] std::optional<PlaylistPlayerUiTarget> CurrentAccountPlaylistTarget(
	not_null<Main::Session*> session,
	const AudioMsgId &current);

void PlayAccountPlaylist(
	not_null<Main::Session*> session,
	const QString &playlistId,
	std::optional<uint64_t> documentId = std::nullopt);
void RetryAccountPlaylistPlayback(not_null<Main::Session*> session);
void StopAccountPlaylistPlayback(not_null<Main::Session*> session);
void CancelPlaylistStartsForSleepTimer();
[[nodiscard]] bool ResumeAccountPlaylistAfterSleepTimer(
	AudioMsgId::Type type,
	const AudioMsgId &current,
	bool restart);
void InvalidatePlaylistPlayback();
void FinishPlaylistPlayback();
[[nodiscard]] bool AppendAccountMusicQueue(
	not_null<Main::Session*> session,
	const std::vector<PlaylistMessage> &messages);

[[nodiscard]] rpl::producer<PlaylistPlaybackStatus> AccountPlaylistPlaybackValue(
	not_null<Main::Session*> session);

[[nodiscard]] std::optional<bool> MoveInAccountPlaylist(
	AudioMsgId::Type type,
	const AudioMsgId &current,
	int delta,
	bool autonext);
[[nodiscard]] std::optional<bool> AccountPlaylistStepAvailable(
	AudioMsgId::Type type,
	const AudioMsgId &current,
	int delta);

} // namespace Purple

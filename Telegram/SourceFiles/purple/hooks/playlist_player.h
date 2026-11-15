#pragma once

#include "base/basic_types.h"
#include "data/data_audio_msg_id.h"

namespace Purple {

enum class PlaylistPlayerIntent { Play, Stop };

class PlaylistPlayerInternalCallGuard final {
public:
	PlaylistPlayerInternalCallGuard(
		AudioMsgId::Type type,
		PlaylistPlayerIntent intent);
	~PlaylistPlayerInternalCallGuard();
	PlaylistPlayerInternalCallGuard(const PlaylistPlayerInternalCallGuard&) = delete;
	PlaylistPlayerInternalCallGuard &operator=(const PlaylistPlayerInternalCallGuard&) = delete;

private:
	Fn<void()> _finish;

};

[[nodiscard]] bool AdmitPlaylistPlayerIntent(
	AudioMsgId::Type type,
	PlaylistPlayerIntent intent);

class PlaylistPlayerCallGuard final {
public:
	explicit PlaylistPlayerCallGuard(AudioMsgId::Type type);
	~PlaylistPlayerCallGuard();
	PlaylistPlayerCallGuard(const PlaylistPlayerCallGuard&) = delete;
	PlaylistPlayerCallGuard &operator=(const PlaylistPlayerCallGuard&) = delete;
	[[nodiscard]] bool current() const;

private:
	Fn<bool()> _current;
	Fn<void()> _finish;

};

[[nodiscard]] Fn<bool()> PlaylistPlayerCallbackGuard(AudioMsgId::Type type);
[[nodiscard]] bool DeferPlaylistPlayerPlay(
	const AudioMsgId &audioId,
	Fn<void()> resume);

} // namespace Purple

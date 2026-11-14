#pragma once

#include "base/weak_ptr.h"
#include "purple/purple_playlists.h"

#include <deque>
#include <functional>

namespace Purple {

enum class PlaylistSongStatus {
	Resolving,
	Ready,
	EarlierCopy,
	Deleted,
	NoAccess,
	Changed,
	Restricted,
	Failed,
	InvalidInput,
};

struct PlaylistResolveSong {
	uint64_t documentId = 0;
	PlaylistSource source;
};

struct PlaylistSourceRead {
	PlaylistSongStatus status = PlaylistSongStatus::Failed;
	uint64_t documentId = 0;
};

struct PlaylistSongResolution {
	uint64_t documentId = 0;
	PlaylistSongStatus status = PlaylistSongStatus::Resolving;
	PlaylistSourceRef source;
	std::size_t sourceIndex = 0;
};

struct PlaylistResolverIo {
	using Done = std::function<void(std::vector<PlaylistSourceRead>)>;
	std::function<std::optional<PlaylistSourceRead>(const PlaylistSourceRef&)> cached;
	std::function<void(std::vector<PlaylistSourceRef>, Done)> read;
	std::function<void(std::function<void()>)> defer;
};

[[nodiscard]] bool PlaylistResolvedChannelMatches(
	const PlaylistSourceRef &source,
	PeerToken resolved,
	uint64_t accessHash);

class PlaylistResolveQueue final : public base::has_weak_ptr {
public:
	PlaylistResolveQueue(
		std::vector<PlaylistResolveSong> songs,
		uint64_t selfUserId,
		PlaylistResolverIo io,
		std::function<void(std::vector<PlaylistSongResolution>)> done);
	void start();

private:
	[[nodiscard]] PlaylistSourceRef source(std::size_t index) const;
	void accept(std::size_t index, PlaylistSourceRead read);
	void pump();

	std::vector<PlaylistResolveSong> _songs;
	PlaylistResolverIo _io;
	std::function<void(std::vector<PlaylistSongResolution>)> _done;
	std::vector<PlaylistSongResolution> _results;
	std::deque<std::size_t> _pending;
	uint64_t _epoch = 0;
	bool _started = false;
	bool _reading = false;

};

} // namespace Purple

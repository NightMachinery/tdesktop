#include "purple/purple_playlists_resolve_queue.h"

namespace Purple {
namespace {

bool Valid(const PlaylistResolveSong &song, uint64_t selfUserId) {
	if (!selfUserId || !PeerTokenText({ PeerTokenKind::User, selfUserId })) {
		return false;
	}
	auto playlist = Playlist();
	playlist.title = PlaylistTitleRegister{ u"Playlist"_q, kMaximumPlaylistStamp };
	playlist.deleted = PlaylistDeletedRegister{ false, kMaximumPlaylistStamp };
	playlist.order = PlaylistOrderRegister{ { song.documentId }, kMaximumPlaylistStamp };
	playlist.songs = std::map<uint64_t, PlaylistSongRegister>{
		{ song.documentId, { { song.source }, kMaximumPlaylistStamp } },
	};
	return bool(ValidatePlaylistLibrary(
		{ .playlists = { { u"pl-aaaaaaaaaaaaaaaa"_q, playlist } } }, selfUserId));
}

bool TryFallback(PlaylistSongStatus status) {
	switch (status) {
	case PlaylistSongStatus::Deleted:
	case PlaylistSongStatus::NoAccess:
	case PlaylistSongStatus::Changed:
	case PlaylistSongStatus::Restricted:
		return true;
	default:
		return false;
	}
}

} // namespace

bool PlaylistResolvedChannelMatches(
		const PlaylistSourceRef &source,
		PeerToken resolved,
		uint64_t accessHash) {
	return source.peer.kind == PeerTokenKind::Channel
		&& source.peer == resolved && bool(accessHash)
		&& bool(PeerTokenText(resolved));
}

PlaylistResolveQueue::PlaylistResolveQueue(
		std::vector<PlaylistResolveSong> songs,
		uint64_t selfUserId,
		PlaylistResolverIo io,
		std::function<void(std::vector<PlaylistSongResolution>)> done)
: _songs(std::move(songs))
, _io(std::move(io))
, _done(std::move(done)) {
	_results.reserve(_songs.size());
	for (auto i = std::size_t(0); i != _songs.size(); ++i) {
		_results.push_back({ .documentId = _songs[i].documentId });
		if (Valid(_songs[i], selfUserId) && _io.cached && _io.read && _io.defer) {
			_pending.push_back(i);
		} else {
			_results.back().status = PlaylistSongStatus::InvalidInput;
		}
	}
}

PlaylistSourceRef PlaylistResolveQueue::source(std::size_t index) const {
	const auto &song = _songs[index].source;
	const auto position = _results[index].sourceIndex;
	return position ? song.alt[position - 1]
		: PlaylistSourceRef{ song.peer, song.msg, song.username };
}

void PlaylistResolveQueue::accept(std::size_t index, PlaylistSourceRead read) {
	auto &result = _results[index];
	result.source = source(index);
	if (read.status == PlaylistSongStatus::Ready) {
		read.status = (read.documentId == result.documentId)
			? (result.sourceIndex ? PlaylistSongStatus::EarlierCopy : PlaylistSongStatus::Ready)
			: PlaylistSongStatus::Changed;
	} else if (read.status == PlaylistSongStatus::EarlierCopy
		|| read.status == PlaylistSongStatus::Resolving) {
		read.status = PlaylistSongStatus::Failed;
	}
	result.status = read.status;
	if (TryFallback(read.status) && result.sourceIndex < _songs[index].source.alt.size()) {
		++result.sourceIndex;
		result.status = PlaylistSongStatus::Resolving;
		_pending.push_back(index);
	}
}

void PlaylistResolveQueue::start() {
	if (!std::exchange(_started, true)) {
		pump();
	}
}

void PlaylistResolveQueue::pump() {
	auto batch = std::vector<PlaylistSourceRef>();
	auto indices = std::vector<std::size_t>();
	for (auto count = 0; count != 100 && !_pending.empty(); ++count) {
		const auto index = _pending.front();
		_pending.pop_front();
		const auto ref = source(index);
		const auto cached = _io.cached(ref);
		if (cached) {
			accept(index, *cached);
		} else {
			batch.push_back(ref);
			indices.push_back(index);
		}
	}
	const auto weak = base::make_weak(this);
	const auto next = [weak] {
		if (const auto self = weak.get()) {
			self->pump();
		}
	};
	if (!batch.empty()) {
		const auto epoch = ++_epoch;
		_reading = true;
		_io.read(std::move(batch), [weak, epoch, indices = std::move(indices), next](auto reads) {
			const auto self = weak.get();
			if (!self || !self->_reading || self->_epoch != epoch) {
				return;
			}
			self->_reading = false;
			const auto complete = reads.size() == indices.size();
			for (auto i = std::size_t(0); i != indices.size(); ++i) {
				self->accept(indices[i], complete ? reads[i] : PlaylistSourceRead());
			}
			self->_io.defer(next);
		});
	} else if (!_pending.empty()) {
		_io.defer(next);
	} else if (_done) {
		auto done = std::move(_done);
		done(std::move(_results));
	}
}

} // namespace Purple

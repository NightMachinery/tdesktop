#include "purple/purple_playlists_worker.h"

namespace Purple {

PlaylistAccountWorker::PlaylistAccountWorker(
		PlaylistStoreRoute route,
		std::shared_ptr<std::atomic<bool>> authority)
: _selfUserId(route.account.userId)
, _authority(std::move(authority))
, _store(std::move(route), [authority = _authority](const PlaylistStoreRoute&) {
	return authority && authority->load(std::memory_order_acquire);
}) {
}

PlaylistAccountResult PlaylistAccountWorker::read() {
	if (!_authority || !_authority->load(std::memory_order_acquire)) {
		return { .snapshot = { .status = PlaylistStoreStatus::Changed } };
	}
	auto result = _opened ? _store.read() : _store.open();
	_opened = _opened || bool(result);
	return { .snapshot = std::move(result) };
}

PlaylistAccountResult PlaylistAccountWorker::edit(
		const PlaylistAccountEdit &request) {
	auto result = read();
	if (!result.snapshot) {
		return result;
	} else if (!request.nowMs || request.nowMs > kMaximumPlaylistStamp) {
		result.edit = PlaylistEditResult{ .error = PlaylistEditError::InvalidStamp };
		return result;
	}
	result.edit = apply(*result.snapshot.library, request);
	if (!*result.edit || !result.edit->changed) {
		return result;
	}
	auto state = *result.snapshot.state;
	state.unsyncedSince = state.unsyncedSince
		? std::min(*state.unsyncedSince, request.nowMs)
		: request.nowMs;
	result.write = _store.write(result.snapshot, result.edit->library,
		state, PlaylistStoreWriteKind::LocalEdit);
	result.snapshot = read().snapshot;
	return result;
}

PlaylistEditResult PlaylistAccountWorker::apply(
		const PlaylistLibrary &library,
		const PlaylistAccountEdit &request) const {
	const auto &id = request.playlistId;
	const auto now = request.nowMs;
	switch (request.kind) {
	case PlaylistAccountEditKind::Create:
		return CreatePlaylist(library, request.title, request.entropy, now, _selfUserId);
	case PlaylistAccountEditKind::Copy:
		return CopyPlaylist(library, request.source, request.entropy, now, _selfUserId);
	case PlaylistAccountEditKind::Rename:
		return RenamePlaylist(library, id, request.title, now, _selfUserId);
	case PlaylistAccountEditKind::Delete:
		return DeletePlaylist(library, id, now, _selfUserId);
	case PlaylistAccountEditKind::Restore:
		return RestorePlaylist(library, id, now, _selfUserId);
	case PlaylistAccountEditKind::DeleteForever:
		return DeleteForeverPlaylist(library, id, _selfUserId);
	case PlaylistAccountEditKind::AddSongs:
		return AddPlaylistSongs(library, id, request.songs, now, _selfUserId);
	case PlaylistAccountEditKind::RemoveSongs:
		return RemovePlaylistSongs(library, id, request.documentIds, now, _selfUserId);
	case PlaylistAccountEditKind::ReorderSongs:
		return ReorderPlaylistSongs(library, id, request.documentIds, now, _selfUserId);
	case PlaylistAccountEditKind::Undo:
		return UndoPlaylistOperation(library, request.undo, now, _selfUserId);
	}
	return { .error = PlaylistEditError::InvalidValue };
}

} // namespace Purple

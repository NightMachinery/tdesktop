#include "purple/purple_playlists_worker.h"

#include "purple/purple_playlist_download_files.h"
#include "purple/purple_playlist_folders.h"

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
	}
	if (!request.nowMs || request.nowMs > kMaximumPlaylistStamp) {
		result.edit = PlaylistEditResult{ .error = PlaylistEditError::InvalidStamp };
		return result;
	}
	auto state = *result.snapshot.state;
	const auto &library = *result.snapshot.library;
	if (request.kind == PlaylistAccountEditKind::CreateHyper) {
		auto folders = ReadPlaylistFolders(state);
		auto candidate = folders
			? PrepareHyperPlaylist(library, *folders, request.songs,
				request.entropy, request.nowMs, _selfUserId)
			: std::nullopt;
		if (!candidate || !WritePlaylistFolders(state, candidate->folders)) {
			result.edit = PlaylistEditResult{ .error = PlaylistEditError::InvalidValue };
			return result;
		}
		result.edit = std::move(candidate->edit);
		result.edit->undo = {};
	} else if (request.kind == PlaylistAccountEditKind::SetFolderName
		|| request.kind == PlaylistAccountEditKind::RemoveFolder
		|| request.kind == PlaylistAccountEditKind::MoveToFolder) {
		auto folders = ReadPlaylistFolders(state);
		const auto changed = folders && (request.kind == PlaylistAccountEditKind::SetFolderName
			? SetPlaylistFolderName(*folders, request.folderId, request.title)
			: request.kind == PlaylistAccountEditKind::RemoveFolder
			? RemovePlaylistFolder(*folders, request.folderId)
			: MovePlaylistToFolder(*folders, library, request.playlistId, request.folderId));
		if (!changed || !WritePlaylistFolders(state, *folders)) {
			result.edit = PlaylistEditResult{ .error = PlaylistEditError::InvalidValue };
			return result;
		}
		result.write = _store.writeLocalState(result.snapshot, state);
		result.snapshot = read().snapshot;
		return result;
	} else {
		result.edit = apply(library, request);
		if (*result.edit && result.edit->changed
			&& request.kind == PlaylistAccountEditKind::Create && !request.folderId.isEmpty()) {
			auto folders = ReadPlaylistFolders(state);
			if (!folders || !MovePlaylistToFolder(*folders, result.edit->library,
					result.edit->playlistId, request.folderId)
				|| !WritePlaylistFolders(state, *folders)) {
				result.edit = PlaylistEditResult{ .error = PlaylistEditError::InvalidValue };
				return result;
			}
		}
	}
	if (!*result.edit || !result.edit->changed) {
		if (*result.edit && request.kind == PlaylistAccountEditKind::AddSongs) {
			auto used = recordUse({ request.playlistId, request.nowMs });
			used.edit = std::move(result.edit);
			return used;
		}
		if (*result.edit && request.kind == PlaylistAccountEditKind::CreateHyper) {
			auto used = recordUse({ result.edit->playlistId, request.nowMs });
			used.edit = std::move(result.edit);
			return used;
		}
		return result;
	}
	state.unsyncedSince = state.unsyncedSince
		? std::min(*state.unsyncedSince, request.nowMs)
		: request.nowMs;
	if (request.kind == PlaylistAccountEditKind::AddSongs
		|| request.kind == PlaylistAccountEditKind::Create
		|| request.kind == PlaylistAccountEditKind::CreateHyper
		|| request.kind == PlaylistAccountEditKind::Copy) {
		auto &recent = state.recentUse[result.edit->playlistId];
		recent = std::max(recent, request.nowMs);
	}
	result.write = _store.write(result.snapshot, result.edit->library,
		state, PlaylistStoreWriteKind::LocalEdit);
	result.snapshot = read().snapshot;
	const auto dropsReferences = request.kind == PlaylistAccountEditKind::RemoveSongs
		|| request.kind == PlaylistAccountEditKind::DeleteForever
		|| request.kind == PlaylistAccountEditKind::Undo;
	if (result && dropsReferences
		&& result.snapshot.state->preserved.contains(u"desktop_playlist_downloads"_q)) {
		auto retained = download({ .kind = PlaylistDownloadRequestKind::Prune });
		result.retentionStatus = retained.downloads
			? retained.downloads->status : PlaylistDownloadStatus::Changed;
		if (retained) result.snapshot = std::move(retained.snapshot);
	}
	return result;
}

PlaylistAccountResult PlaylistAccountWorker::recordUse(
		const PlaylistAccountUse &request) {
	auto result = read();
	if (!result.snapshot) {
		return result;
	}

	const auto &library = *result.snapshot.library;
	const auto i = library.playlists.find(request.playlistId);
	if (!request.nowMs || request.nowMs > kMaximumPlaylistStamp
		|| i == end(library.playlists) || i->second.isPartial()
		|| i->second.deleted->value || library.purged.contains(request.playlistId)) {
		result.write = PlaylistStoreWrite{ .status = PlaylistStoreStatus::InvalidWrite };
		return result;
	}
	auto state = *result.snapshot.state;
	auto &recent = state.recentUse[request.playlistId];
	if (recent >= request.nowMs) {
		return result;
	}
	recent = request.nowMs;
	result.write = _store.writeLocalState(result.snapshot, state);
	result.snapshot = read().snapshot;
	return result;
}

PlaylistAccountResult PlaylistAccountWorker::download(
		const PlaylistAccountDownloadRequest &request) {
	if (request.kind == PlaylistDownloadRequestKind::PrepareTransfer
		|| request.kind == PlaylistDownloadRequestKind::CommitTransfer
		|| request.kind == PlaylistDownloadRequestKind::DiscardTransfer) return transfer(request);
	auto result = read();
	if (!result.snapshot) return result;
	result.downloads = ReadPlaylistDownloadState(*result.snapshot.state);
	if (!*result.downloads || request.kind == PlaylistDownloadRequestKind::Read) {
		return result;
	}
	if (request.kind == PlaylistDownloadRequestKind::RemoveNow
		|| request.kind == PlaylistDownloadRequestKind::Prune) {
		const auto references = PlaylistDownloadReferences(*result.snapshot.library);
		const auto pending = result.downloads->state.pending;
		for (const auto &[token, intent] : pending) {
			if (request.kind == PlaylistDownloadRequestKind::RemoveNow
				? intent.playlistId != request.playlistId : references.contains(intent.documentId)) continue;
			auto discarded = transfer({
				.kind = PlaylistDownloadRequestKind::DiscardTransfer,
				.playlistId = intent.playlistId,
				.transferToken = token,
				.documentId = intent.documentId,
			});
			if (!discarded) return discarded;
		}
		result = read();
		if (!result.snapshot) return result;
		result.downloads = ReadPlaylistDownloadState(*result.snapshot.state);
		if (!*result.downloads) return result;
	}
	const auto fileAuthority = [owner = _authority, job = request.jobAuthority,
			needsJob = request.kind == PlaylistDownloadRequestKind::Register] {
		if (!owner->load(std::memory_order_acquire)) return false;
		const auto token = job.lock();
		return !needsJob || (token && token->load(std::memory_order_acquire));
	};
	auto next = result.downloads->state;
	const auto &library = *result.snapshot.library;
	const auto playlist = library.playlists.find(request.playlistId);
	if (request.kind != PlaylistDownloadRequestKind::Prune
		&& (playlist == end(library.playlists) || playlist->second.isPartial()
			|| library.purged.contains(request.playlistId))) {
		result.downloads->status = PlaylistDownloadStatus::Invalid;
		return result;
	}
	switch (request.kind) {
	case PlaylistDownloadRequestKind::Read: return result;
	case PlaylistDownloadRequestKind::Verify: {
		const auto song = playlist->second.songs->find(request.documentId);
		const auto file = next.files.find(request.documentId);
		if (song == end(*playlist->second.songs) || song->second.value.isRemoved()
			|| file == end(next.files)) return result;
		const auto proof = InspectPlaylistKeptFile(_store.directory(), request.documentId,
			file->second.extension, file->second.size, fileAuthority);
		if (proof && proof.file.identity == file->second.identity
			&& proof.file.sha256 == file->second.sha256) result.downloads->verified.insert(request.documentId);
		return result;
	}
	case PlaylistDownloadRequestKind::Preference:
		if (request.preference == PlaylistKeepDownloaded::Inherit) {
			next.preferences.erase(request.playlistId);
		} else {
			next.preferences[request.playlistId] = request.preference;
		}
		break;
	case PlaylistDownloadRequestKind::Register: {
		const auto authority = request.jobAuthority.lock();
		if (!authority || !authority->load(std::memory_order_acquire)) {
			result.downloads->status = PlaylistDownloadStatus::Changed;
			return result;
		}
		const auto &songs = *playlist->second.songs;
		const auto song = songs.find(request.documentId);
		if (playlist->second.deleted->value || song == end(songs)
			|| !song->second.value.source
			|| *song->second.value.source != request.expectedSource) {
			result.downloads->status = PlaylistDownloadStatus::Changed;
			return result;
		}
		const auto proof = InspectPlaylistKeptFile(_store.directory(),
			request.documentId, request.file.extension, request.file.size, fileAuthority);
		if (!proof || request.file.identity.isEmpty()
			|| proof.file.identity != request.file.identity
			|| (!request.file.sha256.isEmpty()
				&& proof.file.sha256 != request.file.sha256)) {
			result.downloads->status = proof ? PlaylistDownloadStatus::Changed : proof.status;
			return result;
		}
		const auto old = next.files.find(request.documentId);
		if (old != end(next.files)
			&& (old->second.extension != proof.file.extension
				|| ((old->second.identity != proof.file.identity
					|| old->second.sha256 != proof.file.sha256)
					&& (!request.replacesMissing || *request.replacesMissing != old->second)))) {
			result.downloads->status = PlaylistDownloadStatus::Busy;
			return result;
		}
		auto entry = proof.file;
		if (old != end(next.files)) entry.preserved = old->second.preserved;
		next.files[request.documentId] = std::move(entry);
	} break;
	case PlaylistDownloadRequestKind::RemoveNow:
	case PlaylistDownloadRequestKind::Prune: {
		const auto ids = PlaylistDownloadRemovalCandidates(library, next,
			request.kind == PlaylistDownloadRequestKind::RemoveNow
				? std::optional(request.playlistId) : std::nullopt);
		for (const auto id : ids) {
			auto fresh = read().snapshot;
			if (!_authority->load(std::memory_order_acquire) || !fresh
				|| fresh.libraryFile.identity != result.snapshot.libraryFile.identity
				|| fresh.libraryFile.bytes != result.snapshot.libraryFile.bytes
				|| fresh.stateFile.identity != result.snapshot.stateFile.identity
				|| fresh.stateFile.bytes != result.snapshot.stateFile.bytes) {
				result.downloads->status = PlaylistDownloadStatus::Changed;
				return result;
			}
			result.snapshot = std::move(fresh);
			const auto removed = RemovePlaylistKeptFile(
				_store.directory(), id, next.files.at(id), fileAuthority);
			if (removed != PlaylistDownloadStatus::Ready) {
				result.downloads->status = removed;
				return result;
			}
			next.files.erase(id);
		}
	} break;
	default:
		result.downloads->status = PlaylistDownloadStatus::Invalid;
		return result;
	}
	auto local = *result.snapshot.state;
	const auto status = SetPlaylistDownloadState(local, next);
	if (status != PlaylistDownloadStatus::Ready) {
		result.downloads->status = status;
		return result;
	}
	if (local != *result.snapshot.state) {
		const auto authority = request.jobAuthority;
		const auto registerFile = request.kind == PlaylistDownloadRequestKind::Register;
		result.write = _store.writeLocalState(result.snapshot, local,
			[authority, registerFile](SyncDurablePoint) {
				const auto token = authority.lock();
				return !registerFile || (token && token->load(std::memory_order_acquire));
			});
		result.snapshot = read().snapshot;
	}
	if (!result.snapshot || (result.write && !*result.write)) {
		result.downloads->status = PlaylistDownloadStatus::IoError;
		return result;
	}
	result.downloads = ReadPlaylistDownloadState(*result.snapshot.state);
	if (*result.downloads && result.downloads->state != next) {
		result.downloads->status = PlaylistDownloadStatus::Changed;
	}
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
	case PlaylistAccountEditKind::CreateHyper:
	case PlaylistAccountEditKind::SetFolderName:
	case PlaylistAccountEditKind::RemoveFolder:
	case PlaylistAccountEditKind::MoveToFolder:
		break;
	}
	return { .error = PlaylistEditError::InvalidValue };
}

} // namespace Purple

#include "purple/purple_playlists_worker.h"

#include "purple/purple_playlist_download_files.h"
#include <QtCore/QUuid>

namespace Purple {

PlaylistAccountResult PlaylistAccountWorker::transfer(
		const PlaylistAccountDownloadRequest &request) {
	auto result = read();
	if (!result.snapshot) return result;
	result.downloads = ReadPlaylistDownloadState(*result.snapshot.state);
	if (!*result.downloads) return result;
	auto next = result.downloads->state;
	const auto authority = request.jobAuthority.lock();
	const auto discard = request.kind == PlaylistDownloadRequestKind::DiscardTransfer;
	const auto current = [&] {
		return _authority->load(std::memory_order_acquire)
			&& (discard || (authority && authority->load(std::memory_order_acquire)));
	};
	const auto fail = [&](PlaylistDownloadStatus status) {
		result.downloads->status = status;
		return result;
	};
	if (!current()) return fail(PlaylistDownloadStatus::Changed);
	if (!discard) {
		const auto &library = *result.snapshot.library;
		const auto i = library.playlists.find(request.playlistId);
		if (i == end(library.playlists) || i->second.isPartial() || i->second.deleted->value
			|| library.purged.contains(request.playlistId)
			|| !i->second.songs->contains(request.documentId)
			|| i->second.songs->at(request.documentId).value.source != std::optional(request.expectedSource)
			|| !request.file.size || !PlaylistDownloadExtensionSafe(request.file.extension)) {
			return fail(PlaylistDownloadStatus::Changed);
		}
	}
	const auto commit = [&] {
		if (!current()) {
			result.downloads->status = PlaylistDownloadStatus::Changed;
			return false;
		}
		auto local = *result.snapshot.state;
		const auto status = SetPlaylistDownloadState(local, next);
		if (status != PlaylistDownloadStatus::Ready) {
			result.downloads->status = status;
			return false;
		}
		if (local != *result.snapshot.state) {
			result.write = _store.writeLocalState(result.snapshot, local,
				[&](SyncDurablePoint) { return current(); });
			result.snapshot = read().snapshot;
		}
		if (!result.snapshot || (result.write && !*result.write)) {
			result.downloads->status = PlaylistDownloadStatus::IoError;
			return false;
		}
		result.downloads = ReadPlaylistDownloadState(*result.snapshot.state);
		if (!*result.downloads || result.downloads->state != next) {
			result.downloads->status = PlaylistDownloadStatus::Changed;
			return false;
		}
		return true;
	};
	auto token = request.transferToken;
	if (request.kind == PlaylistDownloadRequestKind::PrepareTransfer) {
		const auto kept = next.files.find(request.documentId);
		if (kept != end(next.files)) {
			if (kept->second.size != request.file.size) return fail(PlaylistDownloadStatus::Changed);
			const auto proof = InspectPlaylistKeptFile(_store.directory(), request.documentId,
				kept->second.extension, kept->second.size);
			if (proof && proof.file.identity == kept->second.identity
				&& proof.file.sha256 == kept->second.sha256) {
				result.downloads->prepared = PlaylistDownloadPrepared{
					{}, { request.playlistId, request.documentId, kept->second.extension,
						kept->second.size, kept->second.identity, kept->second.sha256, {} }, true, true };
				return result;
			}
			if (SyncPathIdentity(PlaylistKeptFilePath(_store.directory(), request.documentId,
				kept->second.extension)) != "missing") return fail(PlaylistDownloadStatus::Busy);
		}
		for (const auto &[key, intent] : next.pending) {
			if (intent.documentId != request.documentId) continue;
			if (intent.playlistId != request.playlistId || intent.size != request.file.size
				|| intent.extension != request.file.extension) return fail(PlaylistDownloadStatus::Busy);
			token = key;
			break;
		}
		if (token.isEmpty()) {
			token = QUuid::createUuid().toString(QUuid::Id128).toLower();
			next.pending.emplace(token, PlaylistOwnedDownloadIntent{
				request.playlistId, request.documentId, request.file.extension,
				request.file.size, {}, {}, {} });
		}
		const auto owner = _downloadOwners.find(token);
		if (owner != end(_downloadOwners)) {
			const auto active = owner->second.lock();
			if (active && active->load(std::memory_order_acquire)) return fail(PlaylistDownloadStatus::Busy);
		}
		auto &intent = next.pending.at(token);
		if (!intent.identity.isEmpty()) {
			const auto finalPath = PlaylistKeptFilePath(_store.directory(), intent.documentId, intent.extension);
			if (!intent.sha256.isEmpty()) {
				if (!current()) return fail(PlaylistDownloadStatus::Changed);
				const auto recovered = PublishPlaylistStagingFile(_store.directory(), token, intent);
				if (recovered) {
					_downloadOwners[token] = authority;
					result.downloads->prepared = PlaylistDownloadPrepared{ token, intent, true, false };
					return result;
				}
				if (SyncPathIdentity(finalPath) != "missing") return fail(recovered.status);
			}
			const auto proof = InspectPlaylistStagingFile(_store.directory(), token, intent);
			if (proof && (intent.sha256.isEmpty() || proof.file.sha256 == intent.sha256)) {
				_downloadOwners[token] = authority;
				result.downloads->prepared = PlaylistDownloadPrepared{ token, intent, true, false };
				return result;
			}
			if (!current()) return fail(PlaylistDownloadStatus::Changed);
			const auto removed = DiscardPlaylistStagingFile(_store.directory(), token, intent.identity);
			if (removed != PlaylistDownloadStatus::Ready) return fail(removed);
			intent.identity.clear();
			intent.sha256.clear();
		}
		if (SyncPathIdentity(PlaylistStagingFilePath(_store.directory(), token)) != "missing") {
			return fail(PlaylistDownloadStatus::Busy);
		}
		if (!commit()) return result;
		if (!current()) return fail(PlaylistDownloadStatus::Changed);
		const auto created = CreatePlaylistStagingFile(_store.directory(), token);
		if (!created) return fail(created.status);
		intent.identity = created.file.identity;
		if (!commit()) {
			const auto removed = DiscardPlaylistStagingFile(_store.directory(), token, created.file.identity);
			if (removed != PlaylistDownloadStatus::Ready) result.downloads->status = removed;
			return result;
		}
		_downloadOwners[token] = authority;
		result.downloads->prepared = PlaylistDownloadPrepared{ token, intent, false, false };
		return result;
	}
	const auto i = next.pending.find(token);
	if (i == end(next.pending) || i->second.playlistId != request.playlistId
		|| i->second.documentId != request.documentId) return fail(PlaylistDownloadStatus::Changed);
	auto intent = i->second;
	if (!discard && (intent.size != request.file.size
		|| intent.extension != request.file.extension)) return fail(PlaylistDownloadStatus::Changed);
	const auto owner = _downloadOwners.find(token);
	const auto active = owner == end(_downloadOwners)
		? std::shared_ptr<std::atomic<bool>>() : owner->second.lock();
	if (discard) {
		if (active && active->load(std::memory_order_acquire)) return fail(PlaylistDownloadStatus::Busy);
		const auto stage = PlaylistStagingFilePath(_store.directory(), token);
		if (intent.identity.isEmpty()) {
			if (SyncPathIdentity(stage) != "missing") return fail(PlaylistDownloadStatus::Busy);
		} else {
			const auto finalPath = PlaylistKeptFilePath(_store.directory(), intent.documentId, intent.extension);
			if (SyncPathIdentity(finalPath) == intent.identity) {
				if (PlaylistDownloadReferences(*result.snapshot.library, request.playlistId).contains(intent.documentId)) {
					return fail(PlaylistDownloadStatus::Busy);
				}
				const auto proof = PublishPlaylistStagingFile(_store.directory(), token, intent);
				if (!proof) return fail(proof.status);
				const auto removed = RemovePlaylistKeptFile(_store.directory(), intent.documentId, proof.file);
				if (removed != PlaylistDownloadStatus::Ready) return fail(removed);
			} else {
				const auto removed = DiscardPlaylistStagingFile(_store.directory(), token, intent.identity);
				if (removed != PlaylistDownloadStatus::Ready) return fail(removed);
			}
		}
		next.pending.erase(token);
		_downloadOwners.erase(token);
		if (!commit()) return result;
		return result;
	}
	if (!active || active != authority || !current()) return fail(PlaylistDownloadStatus::Changed);
	if (intent.sha256.isEmpty()) {
		const auto proof = InspectPlaylistStagingFile(_store.directory(), token, intent);
		if (!proof) return fail(proof.status);
		intent.sha256 = proof.file.sha256;
		next.pending[token] = intent;
		if (!commit()) return result;
	}
	if (!current()) return fail(PlaylistDownloadStatus::Changed);
	const auto proof = PublishPlaylistStagingFile(_store.directory(), token, intent);
	if (!proof) return fail(proof.status);
	auto entry = proof.file;
	const auto old = next.files.find(intent.documentId);
	if (old != end(next.files)) entry.preserved = old->second.preserved;
	next.files[intent.documentId] = std::move(entry);
	next.pending.erase(token);
	if (!commit()) return result;
	_downloadOwners.erase(token);
	return result;
}

} // namespace Purple

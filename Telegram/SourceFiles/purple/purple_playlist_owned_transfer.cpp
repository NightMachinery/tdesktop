#include "purple/purple_playlist_owned_transfer.h"

#include "purple/purple_sync_local_store.h"
#include "purple/purple_playlist_download_registry.h"
#include "crl/crl_on_main.h"
#include "data/data_document.h"
#include "media/streaming/media_streaming_loader.h"
#include "media/streaming/media_streaming_reader.h"
#include "storage/streamed_file_downloader.h"
#include <QtCore/QFileInfo>
#include <limits>

namespace Purple {

PlaylistOwnedTransfer::PlaylistOwnedTransfer(
		not_null<DocumentData*> document, Data::FileOrigin origin,
		QString path, QByteArray identity,
		std::shared_ptr<std::atomic<bool>> authority,
		Fn<void(PlaylistDownloadStatus)> done)
: _path(std::move(path))
, _identity(std::move(identity))
, _size(document->size > 0 ? uint64_t(document->size) : 0)
, _authority(std::move(authority))
, _done(std::move(done)) {
	if (!_size || _size > std::numeric_limits<uint32_t>::max()) return;
	_slot = AcquirePlaylistOwnedTransferSlot();
	if (!_slot) {
		_admissionBusy = true;
		return;
	}
	auto loader = document->createStreamingLoader(origin, false);
	if (!loader || loader->size() != document->size) {
		loader = document->createStreamingLoader(origin, true);
	}
	if (!loader || loader->size() != document->size) return;
	_reader = std::make_shared<Media::Streaming::Reader>(std::move(loader), nullptr, true);
	_loader = document->createStreamedDownloader(_reader, origin, std::nullopt,
		_path, LoadToFileOnly, LoadFromCloudOrLocal, false);
}

PlaylistOwnedTransfer::~PlaylistOwnedTransfer() {
	cancel();
}

void PlaylistOwnedTransfer::start() {
	if (_started) return;
	_started = true;
	if (!_loader || !_authority->load(std::memory_order_acquire)
		|| SyncPathIdentity(_path) != _identity || QFileInfo(_path).size() != 0) {
		terminal(_admissionBusy ? PlaylistDownloadStatus::Busy : PlaylistDownloadStatus::Changed);
		return;
	}
	const auto weak = weak_from_this();
	_loader->updates() | rpl::on_next_error_done([] {
	}, [weak](FileLoader::Error) {
		if (const auto self = weak.lock()) self->terminal(PlaylistDownloadStatus::IoError);
	}, [weak] {
		if (const auto self = weak.lock()) self->terminal(PlaylistDownloadStatus::Ready);
	}, _updates);
	_loader->start();
}

void PlaylistOwnedTransfer::terminal(PlaylistDownloadStatus status) {
	if (_queued) return;
	_queued = true;
	const auto self = shared_from_this();
	crl::on_main([self, status]() mutable {
		if (!self->_authority->load(std::memory_order_acquire)) {
			self->release();
			return;
		}
		auto result = status;
		if (status == PlaylistDownloadStatus::Ready
			&& (!self->_loader || !self->_loader->finished() || self->_loader->cancelled()
			|| self->_loader->fullSize() != int64(self->_size)
			|| self->_loader->fileName() != self->_path
			|| SyncPathIdentity(self->_path) != self->_identity
			|| QFileInfo(self->_path).size() != int64(self->_size))) {
			result = PlaylistDownloadStatus::IoError;
		}
		self->release();
		auto done = std::move(self->_done);
		if (done) done(result);
	});
}

void PlaylistOwnedTransfer::release() {
	_updates.destroy();
	if (_loader && !_loader->finished()) _loader->cancel();
	_loader.reset();
	_reader.reset();
	_slot.reset();
}

void PlaylistOwnedTransfer::cancel() {
	_authority->store(false, std::memory_order_release);
	release();
	_done = nullptr;
}

} // namespace Purple

#include "purple/purple_playlist_managed_cache.h"

#include "purple/purple_config.h"
#include "base/weak_ptr.h"
#include "purple/purple_playlist_download_registry.h"
#include "purple/purple_playlist_streaming_source.h"
#include "purple/purple_playlist_cache_bridge.h"
#include "crl/crl_on_main.h"
#include "crl/crl_semaphore.h"
#include "data/data_document.h"
#include "data/data_session.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "media/streaming/media_streaming_reader.h"
#include "storage/cache/storage_cache_database.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QThread>

namespace Purple {

struct PlaylistManagedCacheTransfer::Signals {
	crl::semaphore wake;
	std::atomic<bool> stop = false;
	std::atomic<bool> posted = false;
};

namespace {

PlaylistManagedCacheSource Source(not_null<DocumentData*> document, Data::FileOrigin origin) {
	const auto session = &document->session();
	const auto weak = base::make_weak(session);
	const auto account = SessionAccountKey(session);
	const auto route = CaptureAccountSyncRouting(session);
	return {
		.loader = MakePlaylistStreamingSource(document, origin, true),
		.key = document->bigFileBaseCacheKey(),
		.size = document->size > 0 ? uint64_t(document->size) : 0,
		.cache = &session->data().cacheBigFile(),
		.current = [weak, account, route] {
			const auto session = weak.get();
			return session && route && !session->account().loggingOut()
				&& session->account().maybeSession() == session
				&& SessionAccountKey(session) == account && AccountSyncRoutingCurrent(session, *route);
		},
	};
}

} // namespace

PlaylistManagedCacheTransfer::PlaylistManagedCacheTransfer(
		not_null<DocumentData*> document, Data::FileOrigin origin,
		std::shared_ptr<std::atomic<bool>> authority,
		Fn<void(PlaylistDownloadStatus)> done)
: PlaylistManagedCacheTransfer(Source(document, origin), std::move(authority), std::move(done)) {
}

PlaylistManagedCacheTransfer::PlaylistManagedCacheTransfer(
		PlaylistManagedCacheSource source,
		std::shared_ptr<std::atomic<bool>> authority,
		Fn<void(PlaylistDownloadStatus)> done)
: _current(std::move(source.current))
, _key(source.key)
, _size(source.size)
, _authority(std::move(authority))
, _done(std::move(done))
, _cache(source.cache)
, _source(std::move(source.loader))
, _signals(std::make_shared<Signals>()) {
	if (!PlaylistStreamingSizeSupported(_size) || !_key.valid() || !_cache || !_source
		|| _source->size() != int64(_size) || _source->baseCacheKey() != _key) {
		_admission = PlaylistDownloadStatus::Unsupported;
		return;
	}
	if (!current()) return;
	_slot = AcquirePlaylistOwnedTransferSlot();
	if (!_slot) {
		_admission = PlaylistDownloadStatus::Busy;
		return;
	}
	_buffer.resize(int(kPlaylistStreamingPartBytes));
	_admission = PlaylistDownloadStatus::Ready;
}

PlaylistManagedCacheTransfer::~PlaylistManagedCacheTransfer() {
	cancel();
}

bool PlaylistManagedCacheTransfer::current() const {
	Expects(QThread::currentThread() == QCoreApplication::instance()->thread());
	return !_cancelled && _authority->load(std::memory_order_acquire)
		&& _current && _current();
}

void PlaylistManagedCacheTransfer::start() {
	Expects(QThread::currentThread() == QCoreApplication::instance()->thread());
	if (_started) return;
	_started = true;
	if (_admission != PlaylistDownloadStatus::Ready || !current()) {
		finish(_admission);
		return;
	}
	const auto signals = _signals;
	const auto weak = weak_from_this();
	_relay = std::thread([signals, weak] {
		while (true) {
			signals->wake.acquire();
			if (signals->stop.load(std::memory_order_acquire)) return;
			if (signals->posted.exchange(true, std::memory_order_acq_rel)) continue;
			crl::on_main([signals, weak] {
				signals->posted.store(false, std::memory_order_release);
				if (signals->stop.load(std::memory_order_acquire)) return;
				if (const auto self = weak.lock()) self->pump();
			});
		}
	});
	_reader = std::make_shared<Media::Streaming::Reader>(std::move(_source), _cache);
	PlaylistCacheBridge::Adopt(_reader.get());
	_reader->startStreaming();
	wake();
}

void PlaylistManagedCacheTransfer::wake() {
	_signals->wake.release();
}

void PlaylistManagedCacheTransfer::pump() {
	if (_terminal || _barrierPending) return;
	if (!current()) {
		cancel();
		return;
	}
	const auto length = std::min<uint64_t>(_buffer.size(), _size - _offset);
	const auto result = _reader->fill(int64(_offset),
		bytes::make_span(_buffer).subspan(0, length), &_signals->wake);
	using Fill = Media::Streaming::Reader::FillState;
	if (result == Fill::Failed) {
		finish(PlaylistDownloadStatus::IoError);
		return;
	} else if (result != Fill::Success) {
		return;
	}
	const auto bytes = QByteArrayView(_buffer.constData(), qsizetype(length));
	if (!_offset) {
		const auto prefix = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
		if (_phase == Phase::Write) _prefixHash = prefix;
		else if (prefix != _prefixHash) {
			finish(PlaylistDownloadStatus::IoError);
			return;
		}
	}
	if (!_headerDone) {
		_reader->headerDone();
		_headerDone = true;
	}
	if (_phase == Phase::Presence) {
		finish(_reader->fullInCache() ? PlaylistDownloadStatus::Ready : PlaylistDownloadStatus::IoError);
		return;
	}
	_hash.addData(bytes);
	_offset += length;
	if (_offset == _size) {
		passDone();
	} else if (!(_offset % kPlaylistStreamingSliceBytes)) {
		barrier();
	} else {
		wake();
	}
}

void PlaylistManagedCacheTransfer::barrier(Fn<void()> resume) {
	_barrierPending = true;
	const auto weak = weak_from_this();
	_cache->get(_key, [weak, resume = std::move(resume)](QByteArray&&) mutable {
		crl::on_main([weak, resume = std::move(resume)]() mutable {
			const auto self = weak.lock();
			if (!self || self->_terminal) return;
			if (!self->current()) {
				self->cancel();
				return;
			}
			self->_barrierPending = false;
			if (resume) resume();
			else self->wake();
		});
	});
}

void PlaylistManagedCacheTransfer::passDone() {
	const auto hash = _hash.result();
	if (_phase == Phase::Write) {
		_expectedHash = hash;
		_reader->stopStreaming();
		if (!PlaylistCacheBridge::Flush(_reader.get())) {
			finish(PlaylistDownloadStatus::IoError);
			return;
		}
		const auto weak = weak_from_this();
		barrier([weak] {
			if (const auto self = weak.lock()) {
				self->_writesDrained = true;
				self->beginCachePass(Phase::Readback);
			}
		});
	} else if (hash != _expectedHash) {
		finish(PlaylistDownloadStatus::IoError);
	} else {
		beginCachePass(Phase::Presence);
	}
}

void PlaylistManagedCacheTransfer::beginCachePass(Phase phase) {
	releaseReader();
	if (!current()) {
		cancel();
		return;
	}
	_phase = phase;
	_offset = 0;
	_hash.reset();
	_headerDone = false;
	_reader = std::make_shared<Media::Streaming::Reader>(MakePlaylistCacheOnlySource(_key, _size), _cache);
	PlaylistCacheBridge::Adopt(_reader.get());
	_reader->startStreaming();
	wake();
}

void PlaylistManagedCacheTransfer::stopRelay() {
	_signals->stop.store(true, std::memory_order_release);
	_signals->wake.release();
	if (_relay.joinable()) _relay.join();
}

void PlaylistManagedCacheTransfer::releaseReader() {
	if (_reader) {
		_reader->stopStreaming();
		_reader.reset();
	}
}

void PlaylistManagedCacheTransfer::releaseSlot(Fn<void()> ready) {
	if (!_slot || !_cache || _writesDrained) {
		_slot.reset();
		if (ready) ready();
		return;
	}
	auto slot = std::move(_slot);
	_cache->get(_key, [slot = std::move(slot), ready = std::move(ready)](QByteArray&&) mutable {
		slot.reset();
		if (ready) ready();
	});
}

void PlaylistManagedCacheTransfer::finish(PlaylistDownloadStatus status) {
	if (_terminal) return;
	_terminal = true;
	stopRelay();
	releaseReader();
	_source.reset();
	const auto weak = weak_from_this();
	releaseSlot([weak, status] {
		crl::on_main([weak, status] {
			const auto self = weak.lock();
			if (!self || !self->current()) return;
			auto done = std::move(self->_done);
			if (done) done(status);
		});
	});
	_cache = nullptr;
}

void PlaylistManagedCacheTransfer::cancel() {
	Expects(QThread::currentThread() == QCoreApplication::instance()->thread());
	if (_cancelled) return;
	_cancelled = true;
	_authority->store(false, std::memory_order_release);
	_terminal = true;
	stopRelay();
	releaseReader();
	_source.reset();
	releaseSlot();
	_cache = nullptr;
	_done = nullptr;
}

} // namespace Purple

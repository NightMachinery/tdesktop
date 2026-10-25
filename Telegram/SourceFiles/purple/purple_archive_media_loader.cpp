#include "purple/purple_archive_media_loader.h"
#include "storage/cache/storage_cache_types.h"
#include "media/streaming/media_streaming_reader.h"

#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <limits>
#include <algorithm>

namespace Purple {

ArchiveMediaLoader::ArchiveMediaLoader(std::shared_ptr<ArchiveMediaAccess> access)
: _access(std::move(access)) {
}

ArchiveMediaLoader::~ArchiveMediaLoader() = default;

bool ArchiveMediaLoader::CanOpen(uint64_t size) {
	return size && size <= std::numeric_limits<uint32_t>::max() - uint64_t(16 * 1024 * 1024);
}

Storage::Cache::Key ArchiveMediaLoader::baseCacheKey() const {
	return {};
}

int64 ArchiveMediaLoader::size() const {
	return CanOpen(_access->size()) ? int64(_access->size()) : 0;
}

void ArchiveMediaLoader::fail() {
	stop();
	_parts.fire({ Media::Streaming::LoadedPart::kFailedOffset, {} });
}

void ArchiveMediaLoader::load(int64 offset) {
	QMetaObject::invokeMethod(this, [=, this] { request(offset); });
}

void ArchiveMediaLoader::request(int64 offset) {
	if (!_access->current() || !CanOpen(_access->size()) || offset < 0
		|| offset % kPartSize || uint64_t(offset) >= _access->size()) {
		fail();
		return;
	}
	if (_pending.contains(offset)) {
		return;
	}
	const auto ticket = ++_ticket;
	_pending.emplace(offset, Pending{ticket, _priority, false});
	_queued.emplace(std::pair(std::numeric_limits<uint64_t>::max() - _priority, ticket), offset);
	pump();
}

void ArchiveMediaLoader::pump() {
	if (!_access->current()) {
		fail();
		return;
	}
	while (_running < 8 && !_queued.empty()) {
		const auto offset = _queued.begin()->second;
		_queued.erase(_queued.begin());
		auto &pending = _pending.at(offset);
		pending.active = true;
		const auto ticket = pending.ticket;
		++_running;
		const auto length = uint32_t(std::min(uint64_t(kPartSize), _access->size() - offset));
		const auto weak = QPointer<ArchiveMediaLoader>(this);
		_access->read(offset, length, [weak, offset, ticket, length](std::optional<QByteArray> bytes) mutable {
			if (!weak) {
				return;
			}
			QTimer::singleShot(0, weak, [weak, offset, ticket, length, bytes = std::move(bytes)]() mutable {
				--weak->_running;
				const auto i = weak->_pending.find(offset);
				if (i != weak->_pending.end() && i->second.ticket == ticket) {
					weak->_pending.erase(i);
					if (!weak->_access->current() || !bytes || bytes->size() != length) {
						weak->fail();
						return;
					}
					weak->_parts.fire({ offset, std::move(*bytes) });
				}
				if (weak) {
					weak->pump();
				}
			});
		});
	}
}

void ArchiveMediaLoader::cancel(int64 offset) {
	QMetaObject::invokeMethod(this, [=, this] {
		const auto i = _pending.find(offset);
		if (i == _pending.end()) {
			return;
		}
		if (!i->second.active) {
			_queued.erase({std::numeric_limits<uint64_t>::max() - i->second.priority, i->second.ticket});
		}
		_pending.erase(i);
	});
}

void ArchiveMediaLoader::resetPriorities() {
	QMetaObject::invokeMethod(this, [this] {
		if (_priority == std::numeric_limits<uint64_t>::max()) {
			fail();
		} else {
			++_priority;
		}
	});
}

void ArchiveMediaLoader::setPriority(int priority) {
}

void ArchiveMediaLoader::stop() {
	QMetaObject::invokeMethod(this, [this] {
		_pending.clear();
		_queued.clear();
	});
}

void ArchiveMediaLoader::tryRemoveFromQueue() {
}

rpl::producer<Media::Streaming::LoadedPart> ArchiveMediaLoader::parts() const {
	return _parts.events();
}

rpl::producer<Media::Streaming::SpeedEstimate> ArchiveMediaLoader::speedEstimate() const {
	return rpl::never<Media::Streaming::SpeedEstimate>();
}

void ArchiveMediaLoader::attachDownloader(not_null<Storage::StreamedFileDownloader*> downloader) {
	QMetaObject::invokeMethod(this, [this] { fail(); });
}

void ArchiveMediaLoader::clearAttachedDownloader() {
}

}

namespace Media::Streaming {

void Reader::Slices::prepareBoundedFill(uint32 offset, uint32 length,
	uint32 sliceSize, int maximum) {
	if (!_boundedNoCache || _headerMode != HeaderMode::NoCache || isFullInHeader()) {
		return;
	}
	const auto first = offset / sliceSize;
	const auto last = (offset + length - 1) / sliceSize;
	for (auto index = first; index <= last; ++index) {
		markSliceUsed(index);
	}
	while (_usedSlices.size() > maximum) {
		unloadSlice(_data[_usedSlices.front()]);
		_usedSlices.pop_front();
	}
}

bool Reader::Slices::acceptBoundedPart(uint32 offset, uint32 sliceSize) const {
	return !_boundedNoCache || _headerMode != HeaderMode::NoCache || isFullInHeader()
		|| std::find(_usedSlices.begin(), _usedSlices.end(), int(offset / sliceSize)) != _usedSlices.end();
}

}

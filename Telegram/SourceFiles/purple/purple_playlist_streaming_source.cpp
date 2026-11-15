#include "purple/purple_playlist_streaming_source.h"

#include "base/weak_ptr.h"
#include "crl/crl_on_main.h"
#include "data/data_document.h"
#include "media/streaming/media_streaming_loader_local.h"
#include "storage/cache/storage_cache_types.h"
#include <limits>

namespace Purple {
namespace {

class BoundSource final : public Media::Streaming::Loader {
public:
	BoundSource(std::unique_ptr<Loader> loader, Storage::Cache::Key key)
	: _local(dynamic_cast<Media::Streaming::LoaderLocal*>(loader.get()) != nullptr)
	, _loader(std::move(loader))
	, _key(key) {
	}

	Storage::Cache::Key baseCacheKey() const override { return _key; }
	int64 size() const override { return _loader->size(); }
	void load(int64 offset) override { _loader->load(offset); }
	void cancel(int64 offset) override { _loader->cancel(offset); }
	void resetPriorities() override { _loader->resetPriorities(); }
	void setPriority(int priority) override { _loader->setPriority(priority); }
	void stop() override { _loader->stop(); }
	void tryRemoveFromQueue() override { _loader->tryRemoveFromQueue(); }
	rpl::producer<Media::Streaming::LoadedPart> parts() const override {
		return _loader->parts();
	}
	rpl::producer<Media::Streaming::SpeedEstimate> speedEstimate() const override {
		return _loader->speedEstimate();
	}
	void attachDownloader(not_null<Storage::StreamedFileDownloader*> downloader) override {
		if (!_local) _loader->attachDownloader(downloader);
	}
	void clearAttachedDownloader() override {
		if (!_local) _loader->clearAttachedDownloader();
	}

private:
	bool _local = false;
	std::unique_ptr<Loader> _loader;
	Storage::Cache::Key _key;

};

class CacheOnlySource final : public Media::Streaming::Loader, public base::has_weak_ptr {
public:
	CacheOnlySource(Storage::Cache::Key key, uint64_t size)
	: _key(key), _size(size) {
	}

	Storage::Cache::Key baseCacheKey() const override { return _key; }
	int64 size() const override { return int64(_size); }
	void load(int64) override {
		crl::on_main(this, [=] {
			_parts.fire({ Media::Streaming::LoadedPart::kFailedOffset });
		});
	}
	void cancel(int64) override { }
	void resetPriorities() override { }
	void setPriority(int) override { }
	void stop() override { }
	void tryRemoveFromQueue() override { }
	rpl::producer<Media::Streaming::LoadedPart> parts() const override { return _parts.events(); }
	rpl::producer<Media::Streaming::SpeedEstimate> speedEstimate() const override {
		return rpl::never<Media::Streaming::SpeedEstimate>();
	}
	void attachDownloader(not_null<Storage::StreamedFileDownloader*>) override { }
	void clearAttachedDownloader() override { }

private:
	Storage::Cache::Key _key;
	uint64_t _size = 0;
	rpl::event_stream<Media::Streaming::LoadedPart> _parts;

};

} // namespace

bool PlaylistStreamingSizeSupported(uint64_t size) {
	return size > 0 && size <= kPlaylistStreamingMaximumBytes
		&& size <= uint64_t(std::numeric_limits<uint32_t>::max())
			- (kPlaylistStreamingSliceBytes - 1);
}

std::unique_ptr<Media::Streaming::Loader> BindPlaylistStreamingSource(
		std::unique_ptr<Media::Streaming::Loader> loader,
		Storage::Cache::Key key) {
	if (!loader || !PlaylistStreamingSizeSupported(loader->size())) return nullptr;
	const auto original = loader->baseCacheKey();
	if (original.valid() && original != key) return nullptr;
	if (!original.valid() && !dynamic_cast<Media::Streaming::LoaderLocal*>(loader.get())) return nullptr;
	return std::make_unique<BoundSource>(std::move(loader), key);
}

std::unique_ptr<Media::Streaming::Loader> MakePlaylistStreamingSource(
		not_null<DocumentData*> document,
		Data::FileOrigin origin,
		bool managedCache) {
	if (document->size <= 0 || !PlaylistStreamingSizeSupported(document->size)) return nullptr;
	auto loader = document->createStreamingLoader(origin, false);
	if (!loader || loader->size() != document->size) {
		loader = document->createStreamingLoader(origin, true);
	}
	if (!loader || loader->size() != document->size) return nullptr;
	const auto key = managedCache ? document->bigFileBaseCacheKey() : loader->baseCacheKey();
	if (managedCache && !key.valid()) return nullptr;
	return BindPlaylistStreamingSource(std::move(loader), key);
}

std::unique_ptr<Media::Streaming::Loader> MakePlaylistCacheOnlySource(
		Storage::Cache::Key key,
		uint64_t size) {
	return key.valid() && PlaylistStreamingSizeSupported(size)
		? std::make_unique<CacheOnlySource>(key, size) : nullptr;
}

} // namespace Purple

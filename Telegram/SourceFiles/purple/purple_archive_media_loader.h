#pragma once

#include "media/streaming/media_streaming_loader.h"
#include "purple/purple_archive_media_access.h"
#include <QtCore/QObject>
#include <map>

namespace Purple {

class ArchiveMediaLoader final : public QObject, public Media::Streaming::Loader {
public:
	explicit ArchiveMediaLoader(std::shared_ptr<ArchiveMediaAccess> access);
	~ArchiveMediaLoader() override;
	Storage::Cache::Key baseCacheKey() const override;
	int64 size() const override;
	void load(int64 offset) override;
	void cancel(int64 offset) override;
	void resetPriorities() override;
	void setPriority(int priority) override;
	void stop() override;
	void tryRemoveFromQueue() override;
	rpl::producer<Media::Streaming::LoadedPart> parts() const override;
	rpl::producer<Media::Streaming::SpeedEstimate> speedEstimate() const override;
	void attachDownloader(not_null<Storage::StreamedFileDownloader*> downloader) override;
	void clearAttachedDownloader() override;
	[[nodiscard]] static bool CanOpen(uint64_t size);

private:
	void fail();
	void request(int64 offset);
	void pump();
	struct Pending {
		uint64_t ticket = 0;
		uint64_t priority = 0;
		bool active = false;
	};
	std::shared_ptr<ArchiveMediaAccess> _access;
	std::map<int64, Pending> _pending;
	std::map<std::pair<uint64_t, uint64_t>, int64> _queued;
	int _running = 0;
	uint64_t _priority = 0;
	uint64_t _ticket = 0;
	rpl::event_stream<Media::Streaming::LoadedPart> _parts;

};

}

#pragma once

#include "purple/purple_archive_v2_store.h"
#include "purple/purple_archive_v2_media.h"

namespace Purple::ArchiveV2 {

struct MediaTarget {
	QByteArray store;
	MessageIdentity message;
	int64 sequence = 0;
	int64 ordinal = 0;
	QByteArray source;
	MediaKind kind = MediaKind::Other;
	bool timed = false;
	friend bool operator==(const MediaTarget &, const MediaTarget &) = default;
};

struct MediaRequest {
	MediaTarget target;
	ChatClass kind = ChatClass::Unknown;
	QString chat;
	int64 size = 0;
};

class MediaAdmission final {
public:
	MediaAdmission(std::shared_ptr<Lease> lease, QByteArray generation);
	[[nodiscard]] MediaDecision begin(const Policy &policy, MediaRequest request,
		MediaTrigger trigger, bool reference, bool complete);
	[[nodiscard]] MediaDecision opened(const Policy &policy, MediaRequest request, bool reference,
		bool complete, const std::function<void()> &ordinaryOpen);
	[[nodiscard]] std::optional<BlobInput> finish(const Policy &policy, const MediaTarget &target);
	void cancel(const MediaTarget &target);

private:
	std::shared_ptr<Lease> _lease;
	QByteArray _generation;
	std::optional<MediaRequest> _pending;
};

[[nodiscard]] std::optional<BlobInput> CompleteFile(BlobInput input, const QString &path);

}

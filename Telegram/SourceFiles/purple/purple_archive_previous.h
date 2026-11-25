#pragma once

#include "purple/purple_local_choice.h"
#include <functional>

namespace Purple {

struct PreviousArchiveOptions {
	QString archiveRoot;
	QString profileRoot;
	AccountKey account;
	QString profileNamespace;
	QByteArray loginAuthority;
	QByteArray loginAuthKeyIds;
	std::function<bool()> active;
};

struct PreviousArchiveResult {
	QString reason;
	bool retryable = false;
	[[nodiscard]] explicit operator bool() const { return reason.isEmpty(); }
};

class PreviousArchiveReview {
public:
	struct Token {
		AccountKey account;
		QString profile;
		QByteArray profileIdentity;
		QByteArray directoryIdentity;
		QByteArray recordDigest;
		QByteArray fenceStamp;
		QByteArray intentStamp;
		QString authority;
		QByteArray keys;
	};

	[[nodiscard]] int64 bytes() const { return _bytes; }

private:
	friend class PreviousArchive;
	PreviousArchiveReview() = default;
	int64 _bytes = 0;
	Token _token;
};

struct PreviousArchivePreparation {
	std::optional<PreviousArchiveReview> review;
	QString reason;
};

class PreviousArchive final {
public:
	[[nodiscard]] static PreviousArchivePreparation ReviewPreviousArchive(const PreviousArchiveOptions &options);
	[[nodiscard]] static PreviousArchiveResult DeletePreviousArchive(
		const PreviousArchiveOptions &options, const PreviousArchiveReview &review);
	[[nodiscard]] static bool RetryPreviousArchiveDeletions(const PreviousArchiveOptions &profile);
};

}

#pragma once

#include "purple/purple_archive_v2_store.h"

namespace Purple::ArchiveV2 {

class Writer final {
public:
	using Report = std::function<void(StoreEffect, QByteArray)>;
	Writer(StoreOptions options, Report report);
	~Writer();
	Writer(const Writer &) = delete;
	Writer &operator=(const Writer &) = delete;

	[[nodiscard]] StoreEffect post(CapturedEvent event);
	[[nodiscard]] StoreEffect postBlob(BlobInput input);
	using ReadOrCleanup = std::function<void(Store &)>;
	[[nodiscard]] bool query(QByteArray expectedId, ReadOrCleanup operation);
	[[nodiscard]] bool reset(ResetReview review);
	void logout(QByteArray expectedId);

private:
	struct State;
	static void run(std::shared_ptr<State> state, StoreOptions options);
	void stop(std::optional<QByteArray> logout);
	std::shared_ptr<State> _state;
};

}

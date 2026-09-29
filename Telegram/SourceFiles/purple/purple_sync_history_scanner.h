/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_sync_history_page.h"
#include "base/weak_ptr.h"
#include "mtproto/sender.h"

#include <QtCore/QString>

namespace Main {
class Session;
}

namespace Purple {

enum class SyncHistoryScanStatus {
	Complete,
	Cancelled,
	RequestFailed,
	InvalidResponse,
	Stalled,
};

struct SyncHistoryScanResult {
	SyncHistoryScanStatus status = SyncHistoryScanStatus::InvalidResponse;
	std::vector<int32_t> candidateIds;
	uint64_t scannedCount = 0;
	QString error;

	[[nodiscard]] bool complete() const {
		return status == SyncHistoryScanStatus::Complete;
	}
};

class SyncHistoryScanner final : public base::has_weak_ptr {
public:
	SyncHistoryScanner(
			not_null<Main::Session*> session,
			Fn<void(uint64_t)> progress,
			Fn<void(SyncHistoryScanResult)> finished);
	~SyncHistoryScanner();

	SyncHistoryScanner(const SyncHistoryScanner &) = delete;
	SyncHistoryScanner &operator=(const SyncHistoryScanner &) = delete;

	void Start();
	void Cancel();

private:
	void RequestNext();
	void Finish(SyncHistoryScanStatus status, QString error = {});

	not_null<Main::Session*> _session;
	MTP::Sender _api;
	SyncHistoryPages _pages;
	Fn<void(uint64_t)> _progress;
	Fn<void(SyncHistoryScanResult)> _finished;
	mtpRequestId _requestId = 0;
	bool _started = false;
	bool _done = false;

};

} // namespace Purple

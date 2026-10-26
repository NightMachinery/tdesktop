/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_sync_chat.h"
#include "purple/purple_sync_chat_io.h"
#include "purple/purple_account_config.h"
#include "purple/purple_sync_chat_requests.h"

#include "base/weak_ptr.h"
#include "purple/purple_sync_request_sender.h"
#include "purple/purple_sync_inventory.h"

namespace Main {
class Session;
}

namespace Purple {

[[nodiscard]] SyncHistoryMessageMeta SyncHistoryMessageMetaOf(
	const MTPMessage &message);
[[nodiscard]] bool IsSyncHistoryCandidate(const MTPMessage &message);

class SyncHistoryScanner final : public base::has_weak_ptr {
public:
	SyncHistoryScanner(
			not_null<Main::Session*> session,
			Fn<void(uint64_t)> progress,
			Fn<void(SyncHistoryScanResult)> finished,
			Fn<SyncChatMessageOwnership(const MTPMessage &)> owned = {});
	SyncHistoryScanner(
		not_null<Main::Session*> session,
		DesktopSyncChatIo route,
		Fn<void(uint64_t)> progress,
		Fn<void(SyncHistoryScanResult)> finished,
		Fn<bool()> current);
	~SyncHistoryScanner();

	SyncHistoryScanner(const SyncHistoryScanner &) = delete;
	SyncHistoryScanner &operator=(const SyncHistoryScanner &) = delete;

	void Start();
	void Cancel();

private:
	[[nodiscard]] bool Current() const;
	void RequestNext();
	void Finish(SyncHistoryScanStatus status, QString error = {});

	not_null<Main::Session*> _session;
	base::weak_ptr<Main::Session> _sessionGuard;
	std::optional<AccountOperationTarget> _opening;
	std::optional<DesktopSyncChatIo> _route;
	Fn<bool()> _current;
	SyncRequestSender _api;
	SyncHistoryPages _pages;
	DesktopSyncChatHistoryGuard _modernHistory;
	Fn<void(uint64_t)> _progress;
	Fn<void(SyncHistoryScanResult)> _finished;
	mtpRequestId _requestId = 0;
	Fn<SyncChatMessageOwnership(const MTPMessage &)> _owned;
	bool _started = false;
	bool _done = false;

};

} // namespace Purple

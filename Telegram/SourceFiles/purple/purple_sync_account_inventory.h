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

#include "base/weak_ptr.h"
#include "purple/purple_sync_candidate_reader.h"
#include "purple/purple_sync_history_scanner.h"
#include "purple/purple_sync_inventory.h"

#include <memory>

namespace Purple {

class SyncAccountInventory final : public base::has_weak_ptr {
public:
	SyncAccountInventory(
			not_null<Main::Session*> session,
			Fn<void(uint64_t)> progress,
			Fn<void(SyncAccountInventoryResult)> finished,
			Fn<SyncChatMessageOwnership(const MTPMessage &)> owned = {},
		std::optional<AccountRoutingSnapshot> routing = std::nullopt);
	SyncAccountInventory(
		not_null<Main::Session*> session,
		DesktopSyncChatIo route,
		Fn<void(uint64_t)> progress,
		Fn<void(DesktopSyncChatInventory)> finished,
		Fn<bool()> current,
		std::optional<AccountRoutingSnapshot> routing = std::nullopt);
	~SyncAccountInventory();

	SyncAccountInventory(const SyncAccountInventory &) = delete;
	SyncAccountInventory &operator=(const SyncAccountInventory &) = delete;

	void Start();
	void Cancel();

private:
	[[nodiscard]] bool Current() const;
	void ScanFinished(SyncHistoryScanResult result);
	void ReadFinished(SyncCandidateReadResult result);
	void Finish(SyncAccountInventoryStatus status);

	not_null<Main::Session*> _session;
	base::weak_ptr<Main::Session> _sessionGuard;
	std::optional<AccountOperationTarget> _opening;
	std::optional<AccountRoutingSnapshot> _routing;
	std::optional<DesktopSyncChatIo> _route;
	Fn<bool()> _current;
	Fn<void(uint64_t)> _progress;
	Fn<void(SyncAccountInventoryResult)> _finished;
	SyncAccountInventoryResult _result;
	std::vector<DesktopSyncChatRecordReceipt> _receipts;
	std::unique_ptr<SyncHistoryScanner> _scanner;
	std::unique_ptr<SyncCandidateReader> _reader;
	Fn<SyncChatMessageOwnership(const MTPMessage &)> _owned;
	bool _started = false;
	bool _scanFinished = false;
	bool _done = false;

};

} // namespace Purple

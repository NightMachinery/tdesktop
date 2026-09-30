/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

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
			Fn<void(SyncAccountInventoryResult)> finished);
	~SyncAccountInventory();

	SyncAccountInventory(const SyncAccountInventory &) = delete;
	SyncAccountInventory &operator=(const SyncAccountInventory &) = delete;

	void Start();
	void Cancel();

private:
	void ScanFinished(SyncHistoryScanResult result);
	void ReadFinished(SyncCandidateReadResult result);
	void Finish(SyncAccountInventoryStatus status);

	not_null<Main::Session*> _session;
	Fn<void(uint64_t)> _progress;
	Fn<void(SyncAccountInventoryResult)> _finished;
	SyncAccountInventoryResult _result;
	std::unique_ptr<SyncHistoryScanner> _scanner;
	std::unique_ptr<SyncCandidateReader> _reader;
	bool _started = false;
	bool _scanFinished = false;
	bool _done = false;

};

} // namespace Purple

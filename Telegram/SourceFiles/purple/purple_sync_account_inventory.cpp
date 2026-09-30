/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_account_inventory.h"

#include "data/data_user.h"
#include "main/main_session.h"

#include <utility>

namespace Purple {

SyncAccountInventory::SyncAccountInventory(
		not_null<Main::Session*> session,
		Fn<void(uint64_t)> progress,
		Fn<void(SyncAccountInventoryResult)> finished)
: _session(session)
, _progress(std::move(progress))
, _finished(std::move(finished)) {
	_result.accountUserId = peerToUser(session->user()->id).bare;
}

SyncAccountInventory::~SyncAccountInventory() = default;

void SyncAccountInventory::Start() {
	if (_started || _done) {
		return;
	}
	_started = true;
	const auto weak = base::make_weak(this);
	_scanner = std::make_unique<SyncHistoryScanner>(
		_session,
		[weak](uint64_t count) {
			if (const auto self = weak.get(); self && !self->_done) {
				const auto progress = self->_progress;
				if (progress) {
					progress(count);
				}
			}
		},
		[weak](SyncHistoryScanResult result) {
			if (const auto self = weak.get(); self && !self->_done) {
				self->ScanFinished(std::move(result));
			}
		});
	_scanner->Start();
}

void SyncAccountInventory::Cancel() {
	if (_done) {
		return;
	}
	if (_reader) {
		_reader->Cancel();
		return;
	}
	if (_scanFinished) {
		Finish(SyncAccountInventoryStatus::Incomplete);
		return;
	}
	if (_scanner) {
		_scanner->Cancel();
		return;
	}
	Finish(SyncAccountInventoryStatus::Incomplete);
}

void SyncAccountInventory::ScanFinished(SyncHistoryScanResult result) {
	_scanFinished = true;
	_result.scan = std::move(result);
	const auto weak = base::make_weak(this);
	const auto progress = _progress;
	if (progress) {
		progress(_result.scan.scannedCount);
	}
	if (!weak || _done) {
		return;
	}
	if (!_result.scan.complete()) {
		Finish(SyncAccountInventoryStatus::Incomplete);
		return;
	}
	_reader = std::make_unique<SyncCandidateReader>(
		_session,
		_result.scan.candidateIds,
		[weak](SyncCandidateReadResult result) {
			if (const auto self = weak.get(); self && !self->_done) {
				self->ReadFinished(std::move(result));
			}
		});
	_reader->Start();
}

void SyncAccountInventory::ReadFinished(SyncCandidateReadResult result) {
	_result = FinishSyncAccountInventory(
		_result.accountUserId,
		std::move(_result.scan),
		std::move(result));
	Finish(_result.status);
}

void SyncAccountInventory::Finish(SyncAccountInventoryStatus status) {
	if (_done) {
		return;
	}
	_done = true;
	_result.status = status;
	if (auto finished = std::move(_finished)) {
		finished(std::move(_result));
	}
}

} // namespace Purple

/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_account_inventory.h"

#include "data/data_user.h"
#include "main/main_session.h"
#include "purple/purple_config.h"

#include <utility>

namespace Purple {

SyncAccountInventory::SyncAccountInventory(
		not_null<Main::Session*> session,
		Fn<void(uint64_t)> progress,
		Fn<void(SyncAccountInventoryResult)> finished,
		Fn<SyncChatMessageOwnership(const MTPMessage &)> owned)
: _session(session)
, _sessionGuard(base::make_weak(session))
, _opening(AccountSyncInspectionTarget(session))
, _progress(std::move(progress))
, _finished(std::move(finished))
, _owned(std::move(owned)) {
	_result.accountUserId = peerToUser(session->user()->id).bare;
}

SyncAccountInventory::SyncAccountInventory(
		not_null<Main::Session*> session,
		DesktopSyncChatIo route,
		Fn<void(uint64_t)> progress,
		Fn<void(DesktopSyncChatInventory)> finished,
		Fn<bool()> current)
: SyncAccountInventory(session, std::move(progress),
		[route, finished = std::move(finished)](SyncAccountInventoryResult result) {
			if (finished) {
				finished({ route, std::move(result) });
			}
		}) {
	_route = std::move(route);
	_current = std::move(current);
}

bool SyncAccountInventory::Current() const {
	return _sessionGuard && _opening
		&& ValidateAccountSyncTarget(_session, *_opening)
		&& (!_route || (ValidDesktopSyncChatIo(*_route)
			&& _route->peer.origin.account == SessionAccountKey(_session)
			&& _current && _current()));
}

SyncAccountInventory::~SyncAccountInventory() = default;

void SyncAccountInventory::Start() {
	if (_started || _done) {
		return;
	}
	_started = true;
	if (!Current()) {
		Finish(SyncAccountInventoryStatus::Incomplete);
		return;
	}
	const auto weak = base::make_weak(this);
	const auto progress = [weak](uint64_t count) {
		if (const auto self = weak.get(); self && !self->_done) {
			const auto progress = self->_progress;
			if (progress) {
				progress(count);
			}
		}
	};
	const auto finished = [weak](SyncHistoryScanResult result) {
		if (const auto self = weak.get(); self && !self->_done) {
			self->ScanFinished(std::move(result));
		}
	};
	if (_route) {
		_scanner = std::make_unique<SyncHistoryScanner>(
			_session, *_route, progress, finished, _current);
	} else {
		_scanner = std::make_unique<SyncHistoryScanner>(
			_session, progress, finished, _owned);
	}
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
	if (!Current() || !_result.scan.complete()) {
		Finish(SyncAccountInventoryStatus::Incomplete);
		return;
	}
	const auto finished = [weak](SyncCandidateReadResult result) {
		if (const auto self = weak.get(); self && !self->_done) {
			self->ReadFinished(std::move(result));
		}
	};
	if (_route) {
		_reader = std::make_unique<SyncCandidateReader>(
			_session, *_route, _result.scan.candidateIds, finished, _current);
	} else {
		_reader = std::make_unique<SyncCandidateReader>(
			_session, _result.scan.candidateIds, finished, _owned);
	}
	_reader->Start();
}

void SyncAccountInventory::ReadFinished(SyncCandidateReadResult result) {
	if (!Current()) {
		Finish(SyncAccountInventoryStatus::Incomplete);
		return;
	}
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

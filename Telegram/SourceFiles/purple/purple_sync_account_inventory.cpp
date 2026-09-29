/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_account_inventory.h"

#include <QtCore/QCryptographicHash>

#include <utility>

namespace Purple {
namespace {

[[nodiscard]] SyncDirectoryCandidate DirectoryCandidate(
		const SyncCandidateRecord &record) {
	auto status = SyncEnvelopeStatus::Invalid;
	auto payloadValidated = false;
	switch (record.status) {
	case SyncCandidateStatus::Valid:
		status = SyncEnvelopeStatus::Valid;
		payloadValidated = true;
		break;
	case SyncCandidateStatus::UnsupportedLibrary:
		status = SyncEnvelopeStatus::Valid;
		break;
	case SyncCandidateStatus::UnsupportedStream:
		status = SyncEnvelopeStatus::UnsupportedStream;
		break;
	case SyncCandidateStatus::UnsupportedEncoding:
		status = SyncEnvelopeStatus::UnsupportedEncoding;
		break;
	case SyncCandidateStatus::NewerMajor:
		status = SyncEnvelopeStatus::NewerMajor;
		break;
	default:
		break;
	}
	return {
		.messageId = record.id,
		.documentId = record.documentId,
		.editDate = record.editDate,
		.original = true,
		.status = status,
		.header = record.header,
		.recordHash = record.header && !record.bytes.isEmpty()
			? QString::fromLatin1(QCryptographicHash::hash(
				record.bytes, QCryptographicHash::Sha256).toHex())
			: QString(),
		.payloadValidated = payloadValidated,
	};
}

} // namespace

SyncAccountInventory::SyncAccountInventory(
		not_null<Main::Session*> session,
		Fn<void(uint64_t)> progress,
		Fn<void(SyncAccountInventoryResult)> finished)
: _session(session)
, _progress(std::move(progress))
, _finished(std::move(finished)) {
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
	_result.read = std::move(result);
	auto candidates = std::vector<SyncDirectoryCandidate>();
	candidates.reserve(_result.read->records.size());
	for (const auto &record : _result.read->records) {
		candidates.push_back(DirectoryCandidate(record));
	}
	_result.directory = ResolveSyncDirectory(
		candidates,
		_result.scan.complete()
			&& _result.read->status != SyncCandidateReadStatus::Incomplete);
	const auto directoryNeedsReview = _result.directory.unreadableCandidate
		|| _result.directory.messageIdCollision
		|| (_result.directory.selectedSpace
			&& !_result.directory.publishableSpace);
	auto status = SyncAccountInventoryStatus::Incomplete;
	if (_result.scan.complete()
		&& _result.read->status != SyncCandidateReadStatus::Incomplete) {
		status = (_result.read->status == SyncCandidateReadStatus::Complete
			&& !directoryNeedsReview)
			? SyncAccountInventoryStatus::Complete
			: SyncAccountInventoryStatus::NeedsReview;
	}
	Finish(status);
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

/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_history_scanner.h"

#include "data/data_types.h"
#include "data/data_user.h"
#include "main/main_session.h"

namespace Purple {

SyncHistoryMessageMeta SyncHistoryMessageMetaOf(const MTPMessage &message) {
	auto result = SyncHistoryMessageMeta();
	if (message.type() != mtpc_message) {
		return result;
	}
	result.isMessage = true;
	const auto &data = message.c_message();
	result.forwarded = static_cast<bool>(data.vfwd_from());
	result.caption = qs(data.vmessage());
	const auto media = data.vmedia();
	if (!media || media->type() != mtpc_messageMediaDocument) {
		return result;
	}
	const auto document = media->c_messageMediaDocument().vdocument();
	if (!document || document->type() != mtpc_document) {
		return result;
	}
	result.isDocument = true;
	for (const auto &attribute : document->c_document().vattributes().v) {
		if (attribute.type() == mtpc_documentAttributeFilename) {
			result.fileNames.push_back(
				qs(attribute.c_documentAttributeFilename().vfile_name()));
		}
	}
	return result;
}

bool IsSyncHistoryCandidate(const MTPMessage &message) {
	return IsSyncHistoryCandidate(SyncHistoryMessageMetaOf(message));
}

SyncHistoryScanner::SyncHistoryScanner(
		not_null<Main::Session*> session,
		Fn<void(uint64_t)> progress,
		Fn<void(SyncHistoryScanResult)> finished)
: _session(session)
, _api(&session->mtp())
, _progress(std::move(progress))
, _finished(std::move(finished)) {
}

SyncHistoryScanner::~SyncHistoryScanner() {
	if (_requestId) {
		_api.request(_requestId).cancel();
	}
}

void SyncHistoryScanner::Start() {
	if (_started || _done) {
		return;
	}
	_started = true;
	RequestNext();
}

void SyncHistoryScanner::Cancel() {
	if (_done) {
		return;
	}
	if (_requestId) {
		_api.request(base::take(_requestId)).cancel();
	}
	Finish(SyncHistoryScanStatus::Cancelled);
}

void SyncHistoryScanner::RequestNext() {
	const auto weak = base::make_weak(this);
	_requestId = _api.request(MTPmessages_GetHistory(
		_session->user()->input(),
		MTP_int(_pages.offset()),
		MTP_int(0),
		MTP_int(0),
		MTP_int(kSyncHistoryPageSize),
		MTP_int(0),
		MTP_int(0),
		MTP_long(0)
	)).done([weak](const MTPmessages_Messages &result) {
		const auto self = weak.get();
		if (!self || self->_done) {
			return;
		}
		self->_requestId = 0;
		auto messages = (const QVector<MTPMessage>*)nullptr;
		result.match([&](const MTPDmessages_messagesNotModified &) {
		}, [&](const auto &data) {
			messages = &data.vmessages().v;
		});
		if (!messages) {
			self->Finish(SyncHistoryScanStatus::InvalidResponse);
			return;
		}
		auto page = std::vector<SyncHistoryPageItem>();
		page.reserve(messages->size());
		for (const auto &message : *messages) {
			page.push_back({ IdFromMessage(message).bare, IsSyncHistoryCandidate(message) });
		}
		const auto status = self->_pages.Add(page);
		if (status == SyncHistoryPageStatus::Stalled) {
			self->Finish(SyncHistoryScanStatus::Stalled);
		} else if (status == SyncHistoryPageStatus::Complete) {
			self->Finish(SyncHistoryScanStatus::Complete);
		} else {
			if (self->_progress) {
				self->_progress(self->_pages.count());
			}
			if (const auto live = weak.get(); live && !live->_done) {
				live->RequestNext();
			}
		}
	}).fail([weak](const MTP::Error &error) {
		if (const auto self = weak.get()) {
			self->_requestId = 0;
			self->Finish(SyncHistoryScanStatus::RequestFailed, error.type());
		}
	}).send();
}

void SyncHistoryScanner::Finish(
		SyncHistoryScanStatus status,
		QString error) {
	if (_done) {
		return;
	}
	_done = true;
	auto result = SyncHistoryScanResult{
		.status = status,
		.candidateIds = (status == SyncHistoryScanStatus::Complete)
			? _pages.candidates() : std::vector<int32_t>(),
		.scannedCount = _pages.count(),
		.error = std::move(error),
	};
	if (auto finished = std::move(_finished)) {
		finished(std::move(result));
	}
}

} // namespace Purple

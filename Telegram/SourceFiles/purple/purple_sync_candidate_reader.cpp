/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_candidate_reader.h"

#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_session.h"
#include "data/data_types.h"
#include "data/data_user.h"
#include "main/main_session.h"
#include "purple/purple_sync_history_scanner.h"
#include "rpl/lifetime.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>

namespace Purple {
namespace {

constexpr auto kMaximumRecordBytes = 4 * 1024 * 1024;

[[nodiscard]] SyncCandidateRecord Classify(int32_t id, QByteArray bytes) {
	auto record = SyncCandidateRecord{ .id = id, .bytes = std::move(bytes) };
	const auto parsed = ParseSyncEnvelope(record.bytes);
	record.header = parsed.header;
	record.envelopeError = parsed.error;
	switch (parsed.status) {
	case SyncEnvelopeStatus::NewerMajor:
		record.status = SyncCandidateStatus::NewerMajor;
		return record;
	case SyncEnvelopeStatus::UnsupportedStream:
		record.status = SyncCandidateStatus::UnsupportedStream;
		return record;
	case SyncEnvelopeStatus::UnsupportedEncoding:
		record.status = SyncCandidateStatus::UnsupportedEncoding;
		return record;
	case SyncEnvelopeStatus::Invalid:
		record.status = SyncCandidateStatus::Invalid;
		return record;
	case SyncEnvelopeStatus::Valid:
		break;
	}
	if (parsed.envelope.document.value(u"stream"_q).toString()
			== u"config"_q) {
		const auto inspected = InspectConfigPayload(parsed);
		record.configError = inspected.error;
		record.status = (inspected.status == ConfigPayloadStatus::Valid)
			? SyncCandidateStatus::Valid
			: (inspected.status == ConfigPayloadStatus::NewerSchema)
			? SyncCandidateStatus::NewerSchema
			: SyncCandidateStatus::Invalid;
	} else {
		record.status = SyncCandidateStatus::Valid;
	}
	return record;
}

[[nodiscard]] QByteArray LocalContent(
		const std::shared_ptr<Data::DocumentMedia> &media,
		not_null<DocumentData*> document) {
	if (const auto bytes = media->bytes(); !bytes.isEmpty()) {
		return bytes;
	}
	const auto path = document->filepath(true);
	if (path.isEmpty() || QFileInfo(path).size() > kMaximumRecordBytes) {
		return {};
	}
	auto file = QFile(path);
	return file.open(QIODevice::ReadOnly)
		? file.read(kMaximumRecordBytes + 1) : QByteArray();
}

} // namespace

SyncCandidateReader::SyncCandidateReader(
		not_null<Main::Session*> session,
		std::vector<int32_t> candidateIds,
		Fn<void(SyncCandidateReadResult)> finished)
: _session(session)
, _api(&session->mtp())
, _candidateIds(std::move(candidateIds))
, _finished(std::move(finished)) {
	_result.status = SyncCandidateReadStatus::Complete;
	_result.records.reserve(_candidateIds.size());
}

SyncCandidateReader::~SyncCandidateReader() {
	if (_requestId) {
		_api.request(_requestId).cancel();
	}
}

void SyncCandidateReader::Start() {
	if (_started || _done) {
		return;
	}
	_started = true;
	RequestNext();
}

void SyncCandidateReader::Cancel() {
	if (_done) {
		return;
	}
	if (_requestId) {
		_api.request(base::take(_requestId)).cancel();
	}
	_downloadLifetime.reset();
	_media.reset();
	_document = nullptr;
	for (; _next < _candidateIds.size(); ++_next) {
		_result.records.push_back({
			.id = _candidateIds[_next],
			.status = SyncCandidateStatus::Cancelled,
		});
	}
	_result.status = SyncCandidateReadStatus::Incomplete;
	Finish();
}

void SyncCandidateReader::RequestNext() {
	if (_next == _candidateIds.size()) {
		Finish();
		return;
	}
	const auto id = _candidateIds[_next];
	const auto weak = base::make_weak(this);
	_requestId = _api.request(MTPmessages_GetMessages(
		MTP_vector<MTPInputMessage>(
			1,
			MTP_inputMessageID(MTP_int(id)))
	)).done([weak, id](const MTPmessages_Messages &result) {
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
		if (!messages || messages->size() != 1) {
			self->CompleteCurrent({
				.id = id,
				.status = SyncCandidateStatus::Vanished,
			});
			return;
		}
		const auto &message = messages->front();
		if (IdFromMessage(message).bare != id
			|| message.type() != mtpc_message
			|| PeerFromMessage(message) != self->_session->user()->id
			|| !IsSyncHistoryCandidate(message)) {
			self->CompleteCurrent({
				.id = id,
				.status = SyncCandidateStatus::Changed,
			});
			return;
		}
		const auto media = message.c_message().vmedia();
		const auto document = media->c_messageMediaDocument().vdocument();
		const auto size = document->c_document().vsize().v;
		if (size <= 0 || size > kMaximumRecordBytes) {
			self->CompleteCurrent({
				.id = id,
				.status = (size > kMaximumRecordBytes)
					? SyncCandidateStatus::Oversized
					: SyncCandidateStatus::Invalid,
			});
			return;
		}
		self->ResolveDocument(self->_session->data().processDocument(*document));
	}).fail([weak, id](const MTP::Error &) {
		if (const auto self = weak.get(); self && !self->_done) {
			self->_requestId = 0;
			self->CompleteCurrent({
				.id = id,
				.status = SyncCandidateStatus::RequestFailed,
			});
		}
	}).send();
}

void SyncCandidateReader::ResolveDocument(not_null<DocumentData*> document) {
	_document = document;
	_media = document->createMediaView();
	if (!_media->bytes().isEmpty() || !document->filepath(true).isEmpty()) {
		CheckDownload();
		return;
	}
	document->save(
		FullMsgId(_session->user()->id, _candidateIds[_next]),
		QString());
	if (!document->loading()) {
		CheckDownload();
		return;
	}
	const auto weak = base::make_weak(this);
	_downloadLifetime = std::make_unique<rpl::lifetime>();
	_session->downloaderTaskFinished() | rpl::on_next([weak] {
		if (const auto self = weak.get(); self && !self->_done) {
			self->CheckDownload();
		}
	}, *_downloadLifetime);
}

void SyncCandidateReader::CheckDownload() {
	if (!_document || !_media) {
		return;
	}
	if (_media->bytes().size() > kMaximumRecordBytes) {
		CompleteCurrent({
			.id = _candidateIds[_next],
			.status = SyncCandidateStatus::Oversized,
		});
		return;
	}
	const auto path = _document->filepath(true);
	if (!path.isEmpty() && QFileInfo(path).size() > kMaximumRecordBytes) {
		CompleteCurrent({
			.id = _candidateIds[_next],
			.status = SyncCandidateStatus::Oversized,
		});
		return;
	}
	const auto bytes = LocalContent(_media, _document);
	if (!bytes.isEmpty()) {
		CompleteCurrent(bytes.size() > kMaximumRecordBytes
			? SyncCandidateRecord{
				.id = _candidateIds[_next],
				.status = SyncCandidateStatus::Oversized,
			}
			: Classify(_candidateIds[_next], bytes));
	} else if (!_document->loading()) {
		CompleteCurrent({
			.id = _candidateIds[_next],
			.status = SyncCandidateStatus::Inaccessible,
		});
	}
}

void SyncCandidateReader::CompleteCurrent(SyncCandidateRecord record) {
	_downloadLifetime.reset();
	_media.reset();
	_document = nullptr;
	if (record.status == SyncCandidateStatus::RequestFailed
		|| record.status == SyncCandidateStatus::Inaccessible) {
		_result.status = SyncCandidateReadStatus::Incomplete;
	} else if (record.status != SyncCandidateStatus::Valid
		&& _result.status != SyncCandidateReadStatus::Incomplete) {
		_result.status = SyncCandidateReadStatus::NeedsReview;
	}
	_result.records.push_back(std::move(record));
	++_next;
	RequestNext();
}

void SyncCandidateReader::Finish() {
	if (_done) {
		return;
	}
	_done = true;
	if (auto finished = std::move(_finished)) {
		finished(std::move(_result));
	}
}

} // namespace Purple

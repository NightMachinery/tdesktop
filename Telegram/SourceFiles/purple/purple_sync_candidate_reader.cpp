/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_candidate_reader.h"

#include "data/data_file_origin.h"
#include "data/data_types.h"
#include "data/data_user.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "purple/purple_sync_history_scanner.h"
#include "storage/file_download_mtproto.h"

namespace Purple {
namespace {

constexpr auto kMaximumRecordBytes = kSyncRecordMaximumBytes;

} // namespace

SyncCandidateReader::SyncCandidateReader(
		not_null<Main::Session*> session,
		std::vector<int32_t> candidateIds,
		Fn<void(SyncCandidateReadResult)> finished,
		Fn<SyncChatMessageOwnership(const MTPMessage &)> owned)
: _session(session)
, _sessionGuard(base::make_weak(session))
, _opening(AccountSyncInspectionTarget(session))
, _api(&session->mtp())
, _candidateIds(std::move(candidateIds))
, _finished(std::move(finished))
, _owned(std::move(owned)) {
	_result.records.reserve(_candidateIds.size());
}

SyncCandidateReader::SyncCandidateReader(
		not_null<Main::Session*> session,
		DesktopSyncChatIo route,
		std::vector<int32_t> candidateIds,
		Fn<void(SyncCandidateReadResult)> finished,
		Fn<bool()> current)
: SyncCandidateReader(session, std::move(candidateIds), std::move(finished)) {
	_route = std::move(route);
	_current = std::move(current);
	_owned = [peer = _route->peer](const MTPMessage &message) {
		return GetSyncChatMessageOwnership(
			MapDesktopSyncChatMessage(peer.origin, peer, message));
	};
}

bool SyncCandidateReader::Current() const {
	return _sessionGuard && _opening
		&& ValidateAccountSyncTarget(_session, *_opening)
		&& (!_route || (ValidDesktopSyncChatIo(*_route)
			&& _route->peer.origin.account == SessionAccountKey(_session)
			&& _current && _current()));
}

SyncCandidateReader::~SyncCandidateReader() {
	_done = true;
	_loader = nullptr;
	if (_requestId) {
		_api.request(_requestId).cancel();
	}
}

void SyncCandidateReader::Start() {
	if (_started || _done) {
		return;
	}
	_started = true;
	if (_route) {
		auto ids = std::set<int32_t>();
		for (const auto id : _candidateIds) {
			if (id <= 0 || !ids.emplace(id).second) {
				Finish(SyncCandidateReadStatus::Incomplete);
				return;
			}
		}
	}
	RequestNext();
}

void SyncCandidateReader::Cancel() {
	if (_done) {
		return;
	}
	if (_requestId) {
		_api.request(base::take(_requestId)).cancel();
	}
	_loader = nullptr;
	for (; _next < _candidateIds.size(); ++_next) {
		_result.records.push_back({
			.id = _candidateIds[_next],
			.status = SyncCandidateStatus::Cancelled,
		});
	}
	Finish(SyncCandidateReadStatus::Incomplete);
}

void SyncCandidateReader::RequestNext() {
	if (!Current()) {
		Cancel();
		return;
	}
	if (_next == _candidateIds.size()) {
		Finish(AggregateSyncCandidateRead(_result.records));
		return;
	}
	const auto id = _candidateIds[_next];
	const auto request = _route
		? DesktopSyncChatRecordRequest(*_route, id)
		: std::optional<DesktopSyncChatExactRequest>(MTPmessages_GetMessages(
			MTP_vector<MTPInputMessage>({ MTP_inputMessageID(MTP_int(id)) })));
	if (!request || id <= 0) {
		CompleteCurrent({ .id = id, .status = SyncCandidateStatus::Inaccessible });
		return;
	}
	const auto weak = base::make_weak(this);
	const auto completed = [weak, id](const MTPmessages_Messages &result) {
		const auto self = weak.get();
		if (!self || self->_done) {
			return;
		}
		if (!self->Current()) {
			self->Cancel();
			return;
		}
		self->_requestId = 0;
		auto selected = std::optional<MTPMessage>();
		if (self->_route) {
			selected = DesktopSyncChatRecordMessage(*self->_route, result, id);
		} else if (RecognizedDesktopSyncChatRead(result, PeerToken{})) {
			result.match([](const MTPDmessages_messagesNotModified &) {
			}, [&](const auto &data) {
				if (data.vmessages().v.size() == 1) {
					selected = data.vmessages().v.front();
				}
			});
		}
		if (!selected) {
			self->CompleteCurrent({ .id = id,
				.status = SyncCandidateStatus::Inaccessible });
			return;
		}
		const auto &message = *selected;
		if (IdFromMessage(message).bare != id
			|| message.type() != mtpc_message
			|| (!self->_route && PeerFromMessage(message) != self->_session->user()->id)
			|| !IsSyncHistoryCandidate(message)) {
			self->CompleteCurrent({
				.id = id,
				.status = SyncCandidateStatus::Changed,
			});
			return;
		}
		if (self->_owned) {
			const auto ownership = self->_owned(message);
			if (ownership != SyncChatMessageOwnership::Own) {
				self->CompleteCurrent({
					.id = id,
					.status = (ownership == SyncChatMessageOwnership::Unknown)
						? SyncCandidateStatus::Inaccessible : SyncCandidateStatus::Changed,
				});
				return;
			}
		}
		const auto media = message.c_message().vmedia();
		const auto document = media->c_messageMediaDocument().vdocument();
		if (document->c_document().vid().v <= 0
			|| document->c_document().vdc_id().v <= 0) {
			self->CompleteCurrent({ .id = id,
				.status = SyncCandidateStatus::Inaccessible });
			return;
		}
		const auto documentId = uint64_t(document->c_document().vid().v);
		const auto edit = message.c_message().vedit_date();
		const auto editDate = edit && edit->v > 0 ? uint64_t(edit->v) : 0;
		const auto size = document->c_document().vsize().v;
		if (self->_downloaded) {
			if (documentId != self->_documentId || editDate != self->_editDate
				|| size != self->_expectedBytes) {
				self->CompleteCurrent({ .id = id,
					.status = SyncCandidateStatus::Changed });
			} else {
				self->CompleteCurrent(*base::take(self->_downloaded));
			}
			return;
		}
		self->_documentId = documentId;
		self->_editDate = editDate;
		if (size <= 0 || size > kMaximumRecordBytes) {
			self->CompleteCurrent({
				.id = id,
				.status = (size > kMaximumRecordBytes)
					? SyncCandidateStatus::Oversized
					: SyncCandidateStatus::Invalid,
			});
			return;
		}
		self->StartDownload(document->c_document());
	};
	const auto failed = [weak, id](const MTP::Error &) {
		if (const auto self = weak.get(); self && !self->_done) {
			self->_requestId = 0;
			self->CompleteCurrent({
				.id = id,
				.status = SyncCandidateStatus::RequestFailed,
			});
		}
	};
	std::visit([&](const auto &value) {
		auto copy = value;
		_requestId = _api.request(std::move(copy)).done(completed).fail(failed)
			.handleAllErrors().send();
	}, *request);
}

void SyncCandidateReader::StartDownload(const MTPDdocument &document) {
	if (!Current()) {
		Cancel();
		return;
	}
	const auto size = int64(document.vsize().v);
	_expectedBytes = size;
	_downloadFailed = false;
	const auto origin = _route
		? DesktopSyncChatFileOrigin(*_route, _candidateIds[_next])
		: std::optional(FullMsgId(_session->user()->id,
			MsgId(_candidateIds[_next])));
	if (!origin) {
		CompleteCurrent({ .id = _candidateIds[_next],
			.status = SyncCandidateStatus::Inaccessible });
		return;
	}
	_loader = std::make_unique<mtpFileLoader>(
		_session,
		StorageFileLocation(
			document.vdc_id().v,
			_session->userId(),
			MTP_inputDocumentFileLocation(
				document.vid(),
				document.vaccess_hash(),
				document.vfile_reference(),
				MTP_string())),
		Data::FileOrigin(*origin),
		DocumentFileLocation,
		QString(),
		size,
		size,
		LoadToFileOnly,
		LoadFromCloudOrLocal,
		false,
		0);
	const auto weak = base::make_weak(this);
	const auto stopped = [weak] {
		if (const auto self = weak.get(); self && !self->_done) {
			self->OnDownload();
		}
	};
	_loader->updates(
	) | rpl::on_error_done([=](FileLoader::Error) {
		if (const auto self = weak.get()) {
			self->_downloadFailed = true;
		}
		stopped();
	}, stopped, _loader->lifetime());
	_loader->start();
}

void SyncCandidateReader::OnDownload() {
	if (!Current()) {
		Cancel();
		return;
	}
	if (!_loader) {
		return;
	}
	const auto loader = base::take(_loader);
	const auto id = _candidateIds[_next];
	if (_downloadFailed || loader->cancelled() || loader->bytes().isEmpty()
		|| loader->bytes().size() != _expectedBytes
		|| loader->bytes().size() > kMaximumRecordBytes) {
		CompleteCurrent({
			.id = id,
			.status = SyncCandidateStatus::Inaccessible,
		});
	} else {
		auto record = ClassifySyncCandidate(id, loader->bytes());
		if (_route && record.status == SyncCandidateStatus::Valid) {
			_downloaded = std::move(record);
			RequestNext();
		} else {
			CompleteCurrent(std::move(record));
		}
	}
}

void SyncCandidateReader::CompleteCurrent(SyncCandidateRecord record) {
	record.documentId = _documentId;
	record.editDate = _editDate;
	_documentId = 0;
	_editDate = 0;
	_expectedBytes = 0;
	_downloaded.reset();
	_result.records.push_back(std::move(record));
	++_next;
	RequestNext();
}

void SyncCandidateReader::Finish(SyncCandidateReadStatus status) {
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

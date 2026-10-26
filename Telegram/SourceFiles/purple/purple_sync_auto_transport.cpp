#include "purple/purple_sync_auto_transport.h"

#include "data/data_types.h"
#include "crl/crl_on_main.h"
#include "data/data_user.h"
#include "main/main_session.h"
#include "purple/purple_config.h"

namespace Purple {

DesktopSyncExactMetadata InspectDesktopSyncExactMessage(
		const MTPmessages_Messages &result, int32_t id, uint64_t accountUserId) {
	auto output = DesktopSyncExactMetadata();
	const auto messages = result.match([](const MTPDmessages_messagesNotModified &) {
		return static_cast<const QVector<MTPMessage>*>(nullptr);
	}, [](const auto &data) { return &data.vmessages().v; });
	if (!messages || messages->size() != 1 || id <= 0) { return output; }
	const auto &message = messages->front();
	if (IdFromMessage(message).bare != id) { return output; }
	if (message.type() == mtpc_messageEmpty) {
		const auto peer = message.c_messageEmpty().vpeer_id();
		if (peer && peerFromMTP(*peer) != peerFromUser(UserId(accountUserId))) { return output; }
		output.presence = SyncAutoMessagePresence::Absent;
		return output;
	}
	if (message.type() != mtpc_message || PeerFromMessage(message) != peerFromUser(UserId(accountUserId))) { return output; }
	output.presence = SyncAutoMessagePresence::Present;
	const auto &data = message.c_message();
	output.owned = data.is_out();
	const auto media = data.vmedia();
	if (media && media->type() == mtpc_messageMediaDocument) {
		const auto document = media->c_messageMediaDocument().vdocument();
		if (document && document->type() == mtpc_document) {
			output.documentId = uint64_t(document->c_document().vid().v);
		}
	}
	const auto edit = data.vedit_date();
	output.editDate = edit && edit->v > 0 ? uint64_t(edit->v) : 0;
	return output;
}

DesktopSyncExactMetadata InspectDesktopSyncExactMessage(
		const MTPmessages_Messages &result,
		int32_t id,
		const DesktopSyncChatIo &route) {
	const auto selected = DesktopSyncChatRecordMessage(route, result, id);
	auto output = DesktopSyncExactMetadata();
	if (!selected) {
		return output;
	}
	if (selected->type() == mtpc_messageEmpty) {
		output.presence = SyncAutoMessagePresence::Absent;
		return output;
	}
	if (selected->type() != mtpc_message) {
		return output;
	}
	const auto ownership = GetSyncChatMessageOwnership(
		MapDesktopSyncChatMessage(route.peer.origin, route.peer, *selected));
	if (ownership == SyncChatMessageOwnership::Unknown) {
		return output;
	}
	output.presence = SyncAutoMessagePresence::Present;
	output.owned = ownership == SyncChatMessageOwnership::Own;
	const auto &data = selected->c_message();
	if (const auto media = data.vmedia();
		media && media->type() == mtpc_messageMediaDocument) {
		const auto document = media->c_messageMediaDocument().vdocument();
		if (document && document->type() == mtpc_document) {
			output.documentId = uint64_t(document->c_document().vid().v);
		}
	}
	const auto edit = data.vedit_date();
	output.editDate = edit && edit->v > 0 ? uint64_t(edit->v) : 0;
	return output;
}

DesktopSyncExactRead::DesktopSyncExactRead(
		not_null<Main::Session*> session, int32_t id,
		Fn<bool()> current, Fn<void(SyncAutoServerRead)> finished)
: _session(session)
, _sessionGuard(base::make_weak(session))
, _opening(AccountSyncInspectionTarget(session))
, _id(id)
, _current(std::move(current))
, _finished(std::move(finished))
, _api(&session->mtp()) {
}

DesktopSyncExactRead::DesktopSyncExactRead(
		not_null<Main::Session*> session,
		DesktopSyncChatIo route,
		int32_t id,
		Fn<bool()> current,
		Fn<void(SyncAutoServerRead)> finished)
: DesktopSyncExactRead(session, id, std::move(current), std::move(finished)) {
	_route = std::move(route);
}

bool DesktopSyncExactRead::Current() const {
	return !_done && _sessionGuard && _opening
		&& ValidateAccountSyncTarget(_session, *_opening)
		&& (!_route || (ValidDesktopSyncChatIo(*_route)
			&& _route->peer.origin.account == SessionAccountKey(_session)))
		&& _current && _current();
}

void DesktopSyncExactRead::Start() { Metadata(false); }

void DesktopSyncExactRead::Metadata(bool readback) {
	if (!Current()) { Finish({ .messageId = _id }); return; }
	const auto weak = base::make_weak(this);
	auto request = _route
		? DesktopSyncChatRecordRequest(*_route, _id)
		: std::optional<DesktopSyncChatExactRequest>(MTPmessages_GetMessages(
			MTP_vector<MTPInputMessage>({ MTP_inputMessageID(MTP_int(_id)) })));
	if (!request) {
		Finish({ .messageId = _id });
		return;
	}
	const auto completed = [weak, readback](const MTPmessages_Messages &result) {
		const auto self = weak.get();
		if (!self) { return; }
		if (!self->Current()) { self->Finish({ .messageId = self->_id }); return; }
		const auto metadata = self->_route
			? InspectDesktopSyncExactMessage(result, self->_id, *self->_route)
			: InspectDesktopSyncExactMessage(result, self->_id, self->_session->userId().bare);
		if (metadata.presence == SyncAutoMessagePresence::Absent) {
			self->Finish({ self->_id, {}, true, false, SyncAutoMessagePresence::Absent });
		} else if (readback) {
			const auto same = metadata.presence == SyncAutoMessagePresence::Present
				&& metadata.owned == self->_metadata.owned && metadata.documentId != 0
				&& metadata.documentId == self->_record.documentId
				&& metadata.editDate == self->_record.editDate;
			self->Finish({ self->_id, same ? self->_record.bytes : QByteArray(), same,
				metadata.owned, metadata.presence });
		} else if (metadata.presence != SyncAutoMessagePresence::Present || !metadata.documentId) {
			self->Finish({ self->_id, {}, metadata.presence == SyncAutoMessagePresence::Present,
				metadata.owned, metadata.presence });
		} else {
			self->_metadata = metadata;
			const auto finished = [weak](SyncCandidateReadResult read) {
					const auto self = weak.get();
					if (!self) { return; }
					if (!self->Current() || !read.complete() || read.records.size() != 1
						|| read.records.front().status != SyncCandidateStatus::Valid
						|| read.records.front().bytes.isEmpty()) {
						self->Finish({ .messageId = self->_id }); return;
					}
					self->_record = read.records.front();
					if (self->_record.documentId != self->_metadata.documentId
						|| self->_record.editDate != self->_metadata.editDate) {
						self->Finish({ .messageId = self->_id }); return;
					}
					self->Metadata(true);
				};
			if (self->_route) {
				self->_reader = std::make_unique<SyncCandidateReader>(
					self->_session, *self->_route, std::vector<int32_t>{ self->_id },
					finished, self->_current);
			} else {
				self->_reader = std::make_unique<SyncCandidateReader>(
					self->_session, std::vector<int32_t>{ self->_id }, finished);
			}
			self->_reader->Start();
		}
	};
	const auto failed = [weak](const MTP::Error &) {
		if (const auto self = weak.get()) { self->Finish({ .messageId = self->_id }); }
	};
	std::visit([&](auto value) {
		_api.request(std::move(value)).done(completed).fail(failed)
			.handleAllErrors().send();
	}, *request);
}

void DesktopSyncExactRead::Finish(SyncAutoServerRead read) {
	if (std::exchange(_done, true)) {
		return;
	}
	if (auto finished = std::move(_finished)) {
		crl::on_main([finished = std::move(finished), read = std::move(read)]() mutable { finished(std::move(read)); });
	}
}

}

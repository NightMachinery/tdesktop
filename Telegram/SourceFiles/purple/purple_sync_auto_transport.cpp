#include "purple/purple_sync_auto_transport.h"

#include "data/data_types.h"
#include "crl/crl_on_main.h"
#include "data/data_user.h"
#include "main/main_session.h"

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

DesktopSyncExactRead::DesktopSyncExactRead(
		not_null<Main::Session*> session, int32_t id,
		Fn<bool()> current, Fn<void(SyncAutoServerRead)> finished)
: _session(session)
, _id(id)
, _current(std::move(current))
, _finished(std::move(finished))
, _api(&session->mtp()) {
}

void DesktopSyncExactRead::Start() { Metadata(false); }

void DesktopSyncExactRead::Metadata(bool readback) {
	if (!_current || !_current()) { Finish({ .messageId = _id }); return; }
	const auto weak = base::make_weak(this);
	_api.request(MTPmessages_GetMessages(MTP_vector<MTPInputMessage>(1,
		MTP_inputMessageID(MTP_int(_id))))).done([weak, readback](const MTPmessages_Messages &result) {
		const auto self = weak.get();
		if (!self) { return; }
		if (!self->_current || !self->_current()) { self->Finish({ .messageId = self->_id }); return; }
		const auto metadata = InspectDesktopSyncExactMessage(result, self->_id, self->_session->userId().bare);
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
			self->_reader = std::make_unique<SyncCandidateReader>(self->_session, std::vector<int32_t>{ self->_id },
				[weak](SyncCandidateReadResult read) {
					const auto self = weak.get();
					if (!self) { return; }
					if (read.records.size() != 1 || read.records.front().bytes.isEmpty()) {
						self->Finish({ .messageId = self->_id }); return;
					}
					self->_record = read.records.front();
					if (self->_record.documentId != self->_metadata.documentId
						|| self->_record.editDate != self->_metadata.editDate) {
						self->Finish({ .messageId = self->_id }); return;
					}
					self->Metadata(true);
				});
			self->_reader->Start();
		}
	}).fail([weak](const MTP::Error &) {
		if (const auto self = weak.get()) { self->Finish({ .messageId = self->_id }); }
	}).handleAllErrors().send();
}

void DesktopSyncExactRead::Finish(SyncAutoServerRead read) {
	if (auto finished = std::move(_finished)) {
		crl::on_main([finished = std::move(finished), read = std::move(read)]() mutable { finished(std::move(read)); });
	}
}

}

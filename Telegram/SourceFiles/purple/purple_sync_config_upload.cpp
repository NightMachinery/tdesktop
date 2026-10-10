/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_config_upload.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/random.h"
#include "crl/crl_on_main.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "mtproto/facade.h"
#include "mtproto/mtproto_response.h"
#include "purple/purple_sync_inventory.h"

#include <QtCore/QCryptographicHash>

#include <algorithm>
#include <utility>

namespace Purple {
namespace {

constexpr auto kMaximumUploadBytes = 10 * 1024 * 1024;
constexpr auto kUploadPartBytes = 32 * 1024;

}

SyncConfigUpload::SyncConfigUpload(
		Main::Session &session,
		QByteArray content,
		Fn<void(std::optional<MsgId>)> finished)
: _session(base::make_weak(&session))
, _content(std::move(content))
, _finished(std::move(finished)) {
}

SyncConfigUpload::~SyncConfigUpload() {
	if (_done) {
		return;
	}
	if (auto finished = std::move(_finished)) {
		crl::on_main([finished = std::move(finished)] {
			finished(std::nullopt);
		});
	}
}

void SyncConfigUpload::Start() {
	if (_started || _done) {
		return;
	}
	_started = true;
	const auto session = _session.get();
	if (!session || _content.isEmpty()
			|| _content.size() > kMaximumUploadBytes) {
		Finish(std::nullopt);
		return;
	}
	const auto weak = weak_from_this();
	session->account().sessionChanges(
	) | rpl::on_next([weak, session](Main::Session *current) {
		if (current != session) {
			if (const auto self = weak.lock()) {
				self->_session = nullptr;
				crl::on_main([self] { self->Finish(std::nullopt); });
			}
		}
	}, _lifetime);
	_fileId = base::RandomValue<uint64>();
	_parts = int((_content.size() + kUploadPartBytes - 1)
		/ kUploadPartBytes);
	SendNextPart();
}

void SyncConfigUpload::SendNextPart() {
	if (_done) {
		return;
	}
	const auto session = _session.get();
	if (!session) {
		Finish(std::nullopt);
		return;
	} else if (_offset == _content.size()) {
		SendMedia();
		return;
	}
	const auto size = std::min<qsizetype>(
		kUploadPartBytes,
		_content.size() - _offset);
	const auto bytes = _content.mid(int(_offset), int(size));
	const auto self = shared_from_this();
	session->api().request(MTPupload_SaveFilePart(
		MTP_long(_fileId),
		MTP_int(_part),
		MTP_bytes(bytes)
	)).toDC(MTP::uploadDcId(0)).done([self, size](const MTPBool &result) {
		if (mtpIsFalse(result)) {
			self->Finish(std::nullopt);
			return;
		}
		self->_offset += size;
		++self->_part;
		self->SendNextPart();
	}).fail([self](const MTP::Error &) {
		self->Finish(std::nullopt);
	}).handleAllErrors().send();
}

void SyncConfigUpload::SendMedia() {
	if (_done) {
		return;
	}
	const auto session = _session.get();
	if (!session) {
		Finish(std::nullopt);
		return;
	}
	_randomId = base::RandomValue<uint64>();
	const auto filename = SyncSettingsRecordFileName();
	const auto checksum = QCryptographicHash::hash(
		_content,
		QCryptographicHash::Md5).toHex();
	const auto file = MTP_inputFile(
		MTP_long(_fileId),
		MTP_int(_parts),
		MTP_string(filename),
		MTP_bytes(checksum));
	using MediaFlag = MTPDinputMediaUploadedDocument::Flag;
	const auto mediaFlags = MediaFlag() | MediaFlag::f_force_file;
	const auto media = MTP_inputMediaUploadedDocument(
		MTP_flags(mediaFlags),
		file,
		MTPInputFile(),
		MTP_string(u"application/json"_q),
		MTP_vector<MTPDocumentAttribute>(
			1,
			MTP_documentAttributeFilename(MTP_string(filename))),
		MTP_vector<MTPInputDocument>(),
		MTPInputPhoto(),
		MTP_int(0),
		MTP_int(0));
	const auto history = session->data().history(session->user());
	const auto self = shared_from_this();
	session->api().request(MTPmessages_SendMedia(
		MTP_flags(0),
		history->peer->input(),
		MTPInputReplyTo(),
		media,
		MTP_string(u"#purplesync"_q),
		MTP_long(_randomId),
		MTPReplyMarkup(),
		MTP_vector<MTPMessageEntity>(),
		MTP_int(0),
		MTP_int(0),
		MTPInputPeer(),
		MTPInputQuickReplyShortcut(),
		MTP_long(0),
		MTP_long(0),
		Api::SuggestToMTP({})
	)).done([self](const MTPUpdates &result) {
		self->OnMedia(result);
	}).fail([self](const MTP::Error &) {
		self->Finish(std::nullopt);
	}).handleAllErrors().send();
}

void SyncConfigUpload::OnMedia(const MTPUpdates &result) {
	auto messageId = std::optional<MsgId>();
	const auto inspect = [&](const auto &updates) {
		for (const auto &update : updates.vupdates().v) {
			update.match([&](const MTPDupdateMessageID &data) {
				if (data.vrandom_id().v == _randomId) {
					messageId = MsgId(data.vid().v);
				}
			}, [](const auto &) {});
		}
	};
	result.match(
		[&](const MTPDupdates &data) { inspect(data); },
		[&](const MTPDupdatesCombined &data) { inspect(data); },
		[&](const MTPDupdateShortSentMessage &data) {
			messageId = MsgId(data.vid().v);
		},
		[](const auto &) {});
	if (messageId && !IsServerMsgId(*messageId)) {
		messageId.reset();
	}
	if (const auto session = _session.get()) {
		session->api().applyUpdates(result, _randomId);
	}
	Finish(messageId);
}

void SyncConfigUpload::Finish(std::optional<MsgId> messageId) {
	if (_done) {
		return;
	}
	_done = true;
	_lifetime.destroy();
	if (auto finished = std::move(_finished)) {
		finished(messageId);
	}
}

}

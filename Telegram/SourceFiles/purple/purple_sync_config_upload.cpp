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
#include "purple/purple_config.h"
#include "purple/purple_sync_chat_write.h"

#include <QtCore/QCryptographicHash>

#include <algorithm>
#include <limits>
#include <utility>

namespace Purple {
namespace {

constexpr auto kMaximumUploadBytes = 10 * 1024 * 1024;
constexpr auto kUploadPartBytes = 32 * 1024;

}

SyncConfigUpload::SyncConfigUpload(
		Main::Session &session,
		QByteArray content,
		Fn<void(std::optional<MsgId>)> finished,
		Fn<bool()> stillCurrent, MsgId editId, Fn<void()> editRefused,
		DesktopSyncChatOperation operation)
: _session(base::make_weak(&session))
, _account(SessionAccountKey(&session))
, _operation(std::move(operation))
, _content(std::move(content))
, _finished(std::move(finished))
, _stillCurrent(std::move(stillCurrent))
, _editId(editId)
, _editRefused(std::move(editRefused)) {
}

SyncConfigUpload::SyncConfigUpload(
		Main::Session &session,
		DesktopSyncChatIo route,
		QByteArray content,
		Fn<void(std::optional<MsgId>)> finished,
		Fn<bool()> stillCurrent,
		DesktopSyncChatOperation operation,
		MsgId editId,
		Fn<void()> editRefused)
: SyncConfigUpload(session, std::move(content), std::move(finished),
	std::move(stillCurrent), editId, std::move(editRefused), std::move(operation)) {
	_route = std::move(route);
}

bool SyncConfigUpload::Current() const {
	const auto session = _session.get();
	return !_done && session && !session->account().loggingOut()
		&& session->account().maybeSession() == session
		&& SessionAccountKey(session) == _account
		&& _stillCurrent && _stillCurrent()
		&& (!_route || (ValidDesktopSyncChatIo(*_route)
			&& _route->peer.origin.account == _account))
		&& (!_lease || (_lease->current && _lease->current()));
}

std::optional<DesktopSyncChatIo> SyncConfigUpload::route() const {
	return _lease ? std::optional(_lease->route) : std::nullopt;
}

Fn<bool()> SyncConfigUpload::readbackCurrent() const {
	return _lease ? _lease->current : Fn<bool()>();
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

void SyncConfigUpload::Cancel() {
	Finish(std::nullopt);
}

void SyncConfigUpload::Start() {
	if (_started || _done) {
		return;
	}
	_started = true;
	const auto session = _session.get();
	if (!Current() || _content.isEmpty()
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
	if (!Current()) {
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
	if (_done || _checking) {
		return;
	}
	if (!Current()) {
		Finish(std::nullopt);
		return;
	}
	_checking = true;
	const auto self = shared_from_this();
	const auto chat = _route ? _route->peer.chat : PeerToken{};
	CheckSyncChatBeforePost(_session.get(), [self, chat](DesktopSyncChatStatus status) {
		if (self->_done) {
			return;
		}
		self->_checking = false;
		if (!self->Current() || !status.complete || status.postingHeld
			|| status.phase != DesktopSyncChatPhase::Use
			|| status.resolution.readChat != chat) {
			self->Finish(std::nullopt);
			return;
		}
		const auto lease = SyncChatIoForPost(self->_session.get(), chat);
		if (!lease || !ValidDesktopSyncChatIo(lease->route)
			|| lease->route.peer.origin.account != self->_account
			|| lease->route.peer.chat != chat || !lease->current || !lease->current()
			|| (self->_route && !CompatibleDesktopSyncChatIo(*self->_route, lease->route))) {
			self->Finish(std::nullopt);
			return;
		}
		self->_lease = *lease;
		self->DispatchMedia();
	}, _operation);
}

void SyncConfigUpload::DispatchMedia() {
	if (_done || !Current() || !_lease
		|| _editId.bare < 0 || _editId.bare > std::numeric_limits<int32_t>::max()) {
		Finish(std::nullopt);
		return;
	}
	const auto session = _session.get();
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
	const auto self = shared_from_this();
	if (_editId) {
		auto request = DesktopSyncChatEditRequest(_lease->route, media, int32_t(_editId.bare));
		if (!request) {
			Finish(std::nullopt);
			return;
		}
		session->api().request(std::move(*request)).done([self](const MTPUpdates &result) {
			self->OnMedia(result);
		}).fail([self](const MTP::Error &error) {
			if (!self->Current()) {
				self->Finish(std::nullopt);
				return;
			}
			if (error.type() == u"MESSAGE_NOT_MODIFIED"_q) {
				self->Finish(self->_editId);
				return;
			}
			if (error.type() == u"MESSAGE_EDIT_TIME_EXPIRED"_q || error.type() == u"MESSAGE_AUTHOR_REQUIRED"_q
				|| error.type() == u"MESSAGE_ID_INVALID"_q || error.type() == u"MEDIA_NEW_INVALID"_q) {
				if (auto refused = std::move(self->_editRefused)) {
					self->_done = true;
					self->_finished = nullptr;
					self->_lifetime.destroy();
					refused();
				} else { self->Finish(std::nullopt); }
			} else { self->Finish(std::nullopt); }
		}).handleAllErrors().send();
		return;
	}
	auto request = DesktopSyncChatSendMediaRequest(_lease->route, media, _randomId);
	if (!request) {
		Finish(std::nullopt);
		return;
	}
	session->api().request(std::move(*request)).done([self](const MTPUpdates &result) {
		self->OnMedia(result);
	}).fail([self](const MTP::Error &) {
		self->Finish(std::nullopt);
	}).handleAllErrors().send();
}

void SyncConfigUpload::OnMedia(const MTPUpdates &result) {
	const auto receipt = _lease
		? DesktopSyncChatUploadReceipt(_lease->route, result, _randomId, int32_t(_editId.bare))
		: std::nullopt;
	if (const auto session = _session.get()) {
		session->api().applyUpdates(result, _editId ? uint64_t(0) : _randomId);
	}
	Finish(Current() && receipt ? std::optional(MsgId(*receipt)) : std::nullopt);
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

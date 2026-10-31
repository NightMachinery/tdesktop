/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_config_upload.h"
#include "purple/purple_sync_chat_carry_gate.h"

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
#include "purple/purple_sync_request_sender.h"
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
		DesktopSyncChatOperation operation, Fn<void(Fn<void(bool)>)> authorizeEdit)
: _api(std::make_unique<SyncRequestSender>(&session.mtp(), [this] { return RequestCurrent(); }))
, _session(base::make_weak(&session))
, _account(SessionAccountKey(&session))
, _operation(std::move(operation))
, _content(std::move(content))
, _finished(std::move(finished))
, _stillCurrent(std::move(stillCurrent))
, _editId(editId)
, _editRefused(std::move(editRefused))
, _authorizeEdit(std::move(authorizeEdit)) {
}

SyncConfigUpload::SyncConfigUpload(
		Main::Session &session,
		DesktopSyncChatIo route,
		QByteArray content,
		Fn<void(std::optional<MsgId>)> finished,
		Fn<bool()> stillCurrent,
		DesktopSyncChatOperation operation,
		MsgId editId,
		Fn<void()> editRefused, Fn<void(Fn<void(bool)>)> authorizeEdit)
: SyncConfigUpload(session, std::move(content), std::move(finished),
	std::move(stillCurrent), editId, std::move(editRefused), std::move(operation),
	std::move(authorizeEdit)) {
	_route = std::move(route);
}

SyncConfigUpload::SyncConfigUpload(
		Main::Session &session,
		DesktopSyncCarryPermit permit,
		Fn<void(std::optional<MsgId>)> finished,
		Fn<bool()> stillCurrent,
		Fn<void(Fn<void(std::optional<DesktopSyncCarryPermit>)>)> renewCarry)
: SyncConfigUpload(session, permit.target(), permit.uploadContent(), std::move(finished), std::move(stillCurrent)) {
	_carryContentValid = permit.fileId() && !_content.isEmpty() && permit.claimUpload();
	_fileId = permit.fileId();
	_randomId = permit.randomId();
	_carry = std::move(permit);
	_renewCarry = std::move(renewCarry);
}

bool SyncConfigUpload::RequestCurrent() const {
	if (!_carry || _offset < _content.size()) { return Current(); }
	const auto session = _session.get();
	return !_done && session && !session->account().loggingOut()
		&& session->account().maybeSession() == session
		&& SessionAccountKey(session) == _account;
}

bool SyncConfigUpload::Current() const {
	if (!_carry && DesktopSyncCarryGateFor(_account)->postingHeld()) { return false; }

	const auto session = _session.get();
	return !_done && session && !session->account().loggingOut()
		&& session->account().maybeSession() == session
		&& SessionAccountKey(session) == _account
		&& _stillCurrent && _stillCurrent()
		&& (!_route || (ValidDesktopSyncChatIo(*_route)
			&& _route->peer.origin.account == _account))
		&& (!_lease || (_lease->current && _lease->current()))
		&& (!_carry || (!_editId && _carryContentValid && _carry->current()
			&& SyncChatIoBelongsToSession(session, _carry->target())));
}

std::optional<DesktopSyncChatIo> SyncConfigUpload::route() const {
	return _carry ? std::optional(_carry->target()) : _lease ? std::optional(_lease->route) : std::nullopt;
}

Fn<bool()> SyncConfigUpload::readbackCurrent() const {
	return _carry ? Fn<bool()>([permit = *_carry] { return permit.current(); }) : _lease ? _lease->current : Fn<bool()>();
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
	if (_carry) { _carry->revoke(); }
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
	if (!_carry) { _fileId = base::RandomValue<uint64>(); }
	_parts = int((_content.size() + kUploadPartBytes - 1)
		/ kUploadPartBytes);
	SendNextPart();
}

void SyncConfigUpload::SendNextPart() {
	if (_done) {
		return;
	}
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
	_api->request(MTPupload_SaveFilePart(
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
	const auto self = shared_from_this();
	ResumeEffect([self](bool ready) {
		if (ready) { self->DispatchMedia(); }
		else { self->Finish(std::nullopt); }
	});
}

void SyncConfigUpload::RenewBeforePost(Fn<void(bool)> ready) {
	if (_done || _checking || !Current()) {
		ready(false);
		return;
	}
	_checking = true;
	if (_carry) {
		if (!_renewCarry) { _checking = false; ready(false); return; }
		const auto self = shared_from_this();
		const auto opening = *_carry;
		const auto ticket = ++_carryGateTicket;
		_renewCarry([self, opening, ticket, ready = std::move(ready)](std::optional<DesktopSyncCarryPermit> fresh) {
			if (self->_done || !self->_checking || self->_carryGateTicket != ticket
				|| !self->_carry || self->_carry->renewal() != opening.renewal()) { return; }
			self->_checking = false;
			if (!self->Current() || !fresh || !opening.compatible(*fresh)) { ready(false); return; }
			self->_carry = std::move(fresh);
			self->_route = self->_carry->target();
			ready(true);
		});
		return;
	}
	_lease.reset();
	const auto self = shared_from_this();
	const auto chat = _route ? _route->peer.chat : PeerToken{};
	CheckSyncChatBeforePost(_session.get(), [self, chat, ready = std::move(ready)](DesktopSyncChatStatus status) {
		if (self->_done) { return; }
		self->_checking = false;
		if (!self->Current() || !status.complete || status.postingHeld
			|| status.phase != DesktopSyncChatPhase::Use
			|| status.resolution.readChat != chat) {
			ready(false);
			return;
		}
		const auto lease = SyncChatIoForPost(self->_session.get(), chat);
		if (!lease || !ValidDesktopSyncChatIo(lease->route)
			|| lease->route.peer.origin.account != self->_account
			|| lease->route.peer.chat != chat || !lease->current || !lease->current()
			|| (self->_route && !CompatibleDesktopSyncChatIo(*self->_route, lease->route))) {
			ready(false);
			return;
		}
		self->_lease = *lease;
		ready(true);
	}, _operation);
}

void SyncConfigUpload::ResumeEffect(Fn<void(bool)> ready) {
	const auto self = shared_from_this();
	RenewBeforePost([self, ready = std::move(ready)](bool allowed) mutable {
		if (!allowed || !self->Current()) { ready(false); return; }
		if (self->_operation.authorize) {
			self->_operation.authorize([self, ready = std::move(ready)](bool authorized) mutable {
				if (!authorized || !self->Current()) { ready(false); return; }
				if (!self->_editId) { ready(true); return; }
				if (!self->_authorizeEdit) { ready(false); return; }
				self->_authorizeEdit([self, ready = std::move(ready)](bool allowed) {
					ready(allowed && self->Current());
				});
			});
			return;
		}
		if (!self->_editId) { ready(true); return; }
		if (!self->_authorizeEdit) { ready(false); return; }
		self->_authorizeEdit([self, ready = std::move(ready)](bool authorized) {
			ready(authorized && self->Current());
		});
	});
}

void SyncConfigUpload::DispatchMedia() {
	if (_done || !Current() || (!_lease && !_carry)
		|| _editId.bare < 0 || _editId.bare > std::numeric_limits<int32_t>::max()) {
		Finish(std::nullopt);
		return;
	}
	if (!_carry) { _randomId = base::RandomValue<uint64>(); }
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
		auto request = DesktopSyncChatEditRequest(*route(), media, int32_t(_editId.bare));
		if (!request) {
			Finish(std::nullopt);
			return;
		}
		_api->request(std::move(*request)).done([self](const MTPUpdates &result) {
			self->OnMedia(result);
		}).resumeEffect([self](Fn<void(bool)> ready) {
			self->ResumeEffect(std::move(ready));
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
	auto request = DesktopSyncChatSendMediaRequest(*route(), media, _randomId);
	if (!request) {
		Finish(std::nullopt);
		return;
	}
	_api->request(std::move(*request)).done([self](const MTPUpdates &result) {
		self->OnMedia(result);
	}).resumeEffect([self](Fn<void(bool)> ready) {
		self->ResumeEffect(std::move(ready));
	}).fail([self](const MTP::Error &) {
		self->Finish(std::nullopt);
	}).handleAllErrors().send();
}

std::optional<MTPUpdates> SyncConfigUpload::carryResult() const {
	return _carryResult;
}

void SyncConfigUpload::OnMedia(const MTPUpdates &result) {
	if (_carry) {
		_carryResult = result;
		const auto session = _session;
		const auto receipt = DesktopSyncCarryUpdatesReceipt(*_carry, result,
			[session](const MTPUpdates &updates, uint64_t randomId) {
				if (const auto current = session.get()) { current->api().applyUpdates(updates, randomId); }
			});
		Finish(Current() && receipt ? std::optional(MsgId(*receipt)) : std::nullopt);
		return;
	}
	const auto target = route();
	const auto receipt = target
		? DesktopSyncChatUploadReceipt(*target, result, _randomId, int32_t(_editId.bare))
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
	_api->cancelAll();
	_lifetime.destroy();
	if (auto finished = std::move(_finished)) {
		finished(messageId);
	}
}

}

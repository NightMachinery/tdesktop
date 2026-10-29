/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_config_post.h"
#include "purple/purple_sync_chat_carry_gate.h"

#include "data/data_document.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "purple/purple_config_payload.h"
#include "purple/purple_config.h"
#include "purple/purple_sync_config_upload.h"
#include "purple/purple_sync_inventory.h"
#include "purple/purple_sync_chat_write.h"

#include <cstdint>
#include <map>
#include <limits>
#include <utility>
#include <vector>

namespace Purple {
namespace {

constexpr auto kMaximumStagedBytes = 256 * 1024;

struct PostCounters {
	base::weak_ptr<Main::Session> session;
	uint64_t started = 0;
	int inFlight = 0;
};
std::map<Main::Session*, std::map<QString, std::shared_ptr<PostCounters>>> Posts;

std::shared_ptr<PostCounters> PostsFor(not_null<Main::Session*> session, PeerToken chat = {}) {
	const auto key = PeerTokenText(chat, session->userId().bare).value_or(QString());
	auto &entry = Posts[session.get()][key];
	if (!entry || entry->session.get() != session.get()) {
		entry = std::make_shared<PostCounters>();
		entry->session = base::make_weak(session.get());
	}
	return entry;
}

[[nodiscard]] bool ValidStagedRecord(const QByteArray &staged) {
	if (staged.isEmpty() || staged.size() > kMaximumStagedBytes) {
		return false;
	}
	return AccountSyncRecordSupported(staged);
}

[[nodiscard]] bool HoldsSyncSettingsRecord(not_null<HistoryItem*> item) {
	if (!item->isSending() && !item->hasFailed()) {
		return false;
	}
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	return document
		&& (document->filename() == SyncSettingsRecordFileName());
}

}

SyncConfigSendQueue SyncConfigSendQueueOf(not_null<Main::Session*> session) {
	return SyncConfigSendQueueOf(session, {});
}

SyncConfigSendQueue SyncConfigSendQueueOf(
		not_null<Main::Session*> session,
		PeerToken chat) {
	const auto token = PeerTokenText(chat, session->userId().bare);
	if (!token || ParsePeerToken(*token, session->userId().bare) != chat) {
		return SyncConfigSendQueue::HoldsSyncRecord;
	}
	if (PostsFor(session, chat)->inFlight > 0) {
		return SyncConfigSendQueue::HoldsSyncRecord;
	}
	const auto peer = chat == PeerToken{} ? session->userPeerId()
		: chat.kind == PeerTokenKind::User ? peerFromUser(UserId(chat.id))
		: chat.kind == PeerTokenKind::Chat ? peerFromChat(ChatId(chat.id))
		: peerFromChannel(ChannelId(chat.id));
	const auto history = session->data().historyLoaded(peer);
	if (!history) {
		return SyncConfigSendQueue::Empty;
	}
	for (const auto &item : history->clientSideMessages()) {
		if (HoldsSyncSettingsRecord(item)) {
			return SyncConfigSendQueue::HoldsSyncRecord;
		}
	}
	return SyncConfigSendQueue::Empty;
}

std::function<void()> BeginSyncConfigNetworkOperation(not_null<Main::Session*> session) {
	return BeginSyncConfigNetworkOperation(session, {});
}

std::function<void()> BeginSyncConfigNetworkOperation(
		not_null<Main::Session*> session,
		PeerToken chat) {
	auto writer = DesktopSyncCarryGateFor(SessionAccountKey(session))->writer();
	if (!writer) { return [] {}; }
	const auto counter = PostsFor(session, chat);
	const auto done = std::make_shared<bool>(false);
	++counter->started;
	++counter->inFlight;
	return [counter, done, writer = std::move(writer)]() mutable {
		if (!std::exchange(*done, true)) { --counter->inFlight; writer.reset(); }
	};
}

uint64_t SyncConfigPostsStarted(not_null<Main::Session*> session) {
	return SyncConfigPostsStarted(session, {});
}

uint64_t SyncConfigPostsStarted(not_null<Main::Session*> session, PeerToken chat) {
	return PostsFor(session, chat)->started;
}

SyncConfigPost::SyncConfigPost(
		not_null<Main::Session*> session,
		QByteArray staged,
		Fn<void(SyncConfigPostResult)> finished,
		Fn<bool()> stillCurrent,
		DesktopSyncChatOperation operation)
: _session(session)
, _sessionGuard(base::make_weak(session))
, _staged(std::move(staged))
, _target(operation.routing ? std::nullopt : AccountSyncInspectionTarget(session))
, _finished(std::move(finished))
, _stillCurrent(std::move(stillCurrent))
, _operation(std::move(operation)) {
}

SyncConfigPost::SyncConfigPost(
		not_null<Main::Session*> session,
		DesktopSyncChatIo route,
		QByteArray staged,
		Fn<void(SyncConfigPostResult)> finished,
		Fn<bool()> stillCurrent,
		DesktopSyncChatOperation operation)
: SyncConfigPost(session, std::move(staged), std::move(finished),
	std::move(stillCurrent), std::move(operation)) {
	_route = std::move(route);
}

SyncConfigPost::~SyncConfigPost() {
	_done = true;
	if (_upload) {
		_upload->Cancel();
	}
}

bool SyncConfigPost::ContextCurrent() const {
	return _sessionGuard
		&& !DesktopSyncCarryGateFor(SessionAccountKey(_session))->postingHeld()
		&& (_operation.routing
			? (_operation.authorize && AccountSyncRoutingCurrent(_session, *_operation.routing))
			: (_target && ValidateAccountSyncTarget(_session, *_target)))
		&& _stillCurrent && _stillCurrent()
		&& (!_route || (ValidDesktopSyncChatIo(*_route)
			&& _route->peer.origin.account == SessionAccountKey(_session)))
		&& (!_readbackCurrent || _readbackCurrent());
}

void SyncConfigPost::Start() {
	if (_started || _done) {
		return;
	}
	_started = true;
	if (!ContextCurrent()) {
		Finish({ .status = SyncConfigPostStatus::Cancelled });
		return;
	}
	if (!ValidStagedRecord(_staged)) {
		Finish({ .status = SyncConfigPostStatus::InvalidRecord });
		return;
	}
	const auto weak = base::make_weak(this);
	_posted = true;
	const auto counter = PostsFor(_session, _route ? _route->peer.chat : PeerToken{});
	++counter->started;
	++counter->inFlight;
	const auto finished = [weak, counter](std::optional<MsgId> messageId) {
		--counter->inFlight;
		if (const auto self = weak.get(); self && !self->_done) {
			self->OnReceipt(messageId);
		}
	};
	const auto current = [weak] {
		const auto self = weak.get();
		return self && !self->_done && self->ContextCurrent();
	};
	if (_route) {
		_upload = std::make_shared<SyncConfigUpload>(
			*_sessionGuard.get(), *_route, _staged, finished, current, _operation);
	} else {
		_upload = std::make_shared<SyncConfigUpload>(
			*_sessionGuard.get(), _staged, finished, current, MsgId(0), nullptr, _operation);
	}
	_upload->Start();
}

void SyncConfigPost::Cancel() {
	if (_done) {
		return;
	}
	const auto result = SyncConfigPostResult{
		.status = _posted
			? SyncConfigPostStatus::OutcomeUnknown
			: SyncConfigPostStatus::Cancelled,
		.messageId = _messageId,
	};
	_done = true;
	if (_upload) {
		_upload->Cancel();
	}
	if (_reader) {
		_reader.reset();
	}
	if (auto finished = std::move(_finished)) {
		finished(result);
	}
}

void SyncConfigPost::OnReceipt(std::optional<MsgId> messageId) {
	if (!ContextCurrent()) {
		Finish({ .status = SyncConfigPostStatus::OutcomeUnknown });
		return;
	}
	if (!messageId || messageId->bare <= 0
			|| messageId->bare > std::numeric_limits<int32_t>::max()) {
		Finish({ .status = SyncConfigPostStatus::OutcomeUnknown });
		return;
	}
	const auto route = _upload ? _upload->route() : std::nullopt;
	const auto proof = _upload ? _upload->readbackCurrent() : Fn<bool()>();
	if (!route || !proof || !proof()
		|| (_route && !CompatibleDesktopSyncChatIo(*_route, *route))) {
		Finish({ .status = SyncConfigPostStatus::OutcomeUnknown });
		return;
	}
	_route = *route;
	_readbackCurrent = proof;
	_messageId = *messageId;
	const auto weak = base::make_weak(this);
	_reader = std::make_unique<DesktopSyncExactRead>(
		_session, *_route, int32_t(_messageId.bare),
		[weak] {
			const auto self = weak.get();
			return self && !self->_done && self->ContextCurrent();
		},
		[weak](SyncAutoServerRead result) {
			if (const auto self = weak.get(); self && !self->_done) {
				self->OnReadback(std::move(result));
			}
		}, _operation.routing);
	_reader->Start();
}

void SyncConfigPost::OnReadback(SyncAutoServerRead result) {
	if (!ContextCurrent()) {
		Finish({ .status = SyncConfigPostStatus::OutcomeUnknown });
		return;
	}
	if (result.fresh && result.owned
		&& result.presence == SyncAutoMessagePresence::Present
		&& result.messageId == _messageId.bare && result.record == _staged) {
		Finish({
			.status = SyncConfigPostStatus::Confirmed,
			.messageId = _messageId,
			.readback = std::move(result.record),
		});
	} else {
		Finish({ .status = SyncConfigPostStatus::OutcomeUnknown,
			.messageId = _messageId });
	}
}

void SyncConfigPost::Finish(SyncConfigPostResult result) {
	if (_done) {
		return;
	}
	_done = true;
	if (auto finished = std::move(_finished)) {
		finished(std::move(result));
	}
}

}

/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_config_post.h"

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
std::map<Main::Session*, std::shared_ptr<PostCounters>> Posts;

std::shared_ptr<PostCounters> PostsFor(not_null<Main::Session*> session) {
	auto &entry = Posts[session.get()];
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

SyncConfigSendQueue SyncConfigSendQueueOf(
		not_null<Main::Session*> session) {
	if (PostsFor(session)->inFlight > 0) {
		return SyncConfigSendQueue::HoldsSyncRecord;
	}
	const auto history = session->data().historyLoaded(session->userPeerId());
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
	const auto counter = PostsFor(session);
	const auto done = std::make_shared<bool>(false);
	++counter->started;
	++counter->inFlight;
	return [counter, done] {
		if (!std::exchange(*done, true)) { --counter->inFlight; }
	};
}

uint64_t SyncConfigPostsStarted(not_null<Main::Session*> session) {
	return PostsFor(session)->started;
}

SyncConfigPost::SyncConfigPost(
		not_null<Main::Session*> session,
		QByteArray staged,
		Fn<void(SyncConfigPostResult)> finished,
		Fn<bool()> stillCurrent)
: _session(session)
, _sessionGuard(base::make_weak(session))
, _staged(std::move(staged))
, _target(AccountSyncInspectionTarget(session))
, _finished(std::move(finished))
, _stillCurrent(std::move(stillCurrent)) {
}

SyncConfigPost::~SyncConfigPost() = default;

bool SyncConfigPost::ContextCurrent() const {
	return _sessionGuard && _target && ValidateAccountSyncTarget(_session, *_target)
		&& _stillCurrent && _stillCurrent();
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
	const auto counter = PostsFor(_session);
	++counter->started;
	++counter->inFlight;
	_upload = std::make_shared<SyncConfigUpload>(
		*_sessionGuard.get(),
		_staged,
		[weak, counter](std::optional<MsgId> messageId) {
			--counter->inFlight;
			if (const auto self = weak.get(); self && !self->_done) {
				self->OnReceipt(messageId);
			}
		},
		[weak] {
			const auto self = weak.get();
			return self && !self->_done && self->ContextCurrent();
		});
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
	if (_reader) {
		_reader->Cancel();
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
	_messageId = *messageId;
	const auto weak = base::make_weak(this);
	_reader = std::make_unique<SyncCandidateReader>(
		_session,
		std::vector<int32_t>{ int32_t(_messageId.bare) },
		[weak](SyncCandidateReadResult result) {
			if (const auto self = weak.get(); self && !self->_done) {
				self->OnReadback(std::move(result));
			}
		});
	_reader->Start();
}

void SyncConfigPost::OnReadback(SyncCandidateReadResult result) {
	if (!ContextCurrent()) {
		Finish({ .status = SyncConfigPostStatus::OutcomeUnknown });
		return;
	}
	if (result.status == SyncCandidateReadStatus::Complete
			&& result.records.size() == 1
			&& result.records.front().id == _messageId.bare
			&& result.records.front().status == SyncCandidateStatus::Valid
			&& result.records.front().bytes == _staged) {
		Finish({
			.status = SyncConfigPostStatus::Confirmed,
			.messageId = _messageId,
			.readback = std::move(result.records.front().bytes),
		});
	} else {
		Finish({
			.status = result.status == SyncCandidateReadStatus::Incomplete
				? SyncConfigPostStatus::OutcomeUnknown
				: SyncConfigPostStatus::NeedsReview,
			.messageId = _messageId,
		});
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

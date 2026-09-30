/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_config_post.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "purple/purple_config_payload.h"
#include "purple/purple_sync_inventory.h"
#include "storage/localimageloader.h"
#include "ui/chat/attach/attach_prepare.h"

#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace Purple {
namespace {

constexpr auto kMaximumStagedBytes = 256 * 1024;

uint64_t PostsStarted = 0;
int PostsInFlight = 0;

[[nodiscard]] bool ValidStagedRecord(const QByteArray &staged) {
	if (staged.isEmpty() || staged.size() > kMaximumStagedBytes) {
		return false;
	}
	const auto parsed = ParseSyncEnvelope(staged);
	if (!parsed || InspectConfigPayload(parsed).status
			!= ConfigPayloadStatus::Valid) {
		return false;
	}
	const auto serialized = SerializeSyncEnvelope(parsed.envelope);
	return serialized && serialized.canonical == staged;
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
	if (PostsInFlight > 0) {
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

uint64_t SyncConfigPostsStarted() {
	return PostsStarted;
}

SyncConfigPost::SyncConfigPost(
		not_null<Main::Session*> session,
		QByteArray staged,
		Fn<void(SyncConfigPostResult)> finished)
: _session(session)
, _sessionGuard(base::make_weak(session))
, _staged(std::move(staged))
, _finished(std::move(finished)) {
}

SyncConfigPost::~SyncConfigPost() = default;

void SyncConfigPost::Start() {
	if (_started || _done) {
		return;
	}
	_started = true;
	if (!_sessionGuard) {
		Finish({ .status = SyncConfigPostStatus::Cancelled });
		return;
	}
	if (!ValidStagedRecord(_staged)) {
		Finish({ .status = SyncConfigPostStatus::InvalidRecord });
		return;
	}
	auto file = Ui::PreparedFile(QString());
	file.content = _staged;
	file.displayName = SyncSettingsRecordFileName();
	file.size = _staged.size();
	file.caption = { u"#purplesync"_q };
	file.information = std::make_unique<Ui::PreparedFileInformation>();
	file.information->filemime = u"application/json"_q;

	auto list = Ui::PreparedList();
	list.files.push_back(std::move(file));

	const auto history = _session->data().history(_session->user());
	auto action = Api::SendAction(history);
	action.clearDraft = false;
	const auto weak = base::make_weak(this);
	_posted = true;
	++PostsStarted;
	++PostsInFlight;
	_session->api().sendFiles(
		std::move(list),
		SendMediaType::File,
		nullptr,
		action,
		[weak](std::optional<MsgId> messageId) {
			--PostsInFlight;
			if (const auto self = weak.get(); self && !self->_done) {
				self->OnReceipt(messageId);
			}
		});
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
	if (!_sessionGuard) {
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

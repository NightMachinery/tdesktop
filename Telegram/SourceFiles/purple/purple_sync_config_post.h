/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/weak_ptr.h"
#include "data/data_msg_id.h"
#include "purple/purple_sync_candidate_reader.h"

#include <QtCore/QByteArray>

#include <memory>
#include <optional>

namespace Purple {

enum class SyncConfigPostStatus {
	Confirmed,
	InvalidRecord,
	Cancelled,
	OutcomeUnknown,
	NeedsReview,
};

struct SyncConfigPostResult {
	SyncConfigPostStatus status = SyncConfigPostStatus::OutcomeUnknown;
	MsgId messageId;
	QByteArray readback;
};

class SyncConfigPost final : public base::has_weak_ptr {
public:
	SyncConfigPost(
		not_null<Main::Session*> session,
		QByteArray staged,
		Fn<void(SyncConfigPostResult)> finished);
	~SyncConfigPost();

	SyncConfigPost(const SyncConfigPost &) = delete;
	SyncConfigPost &operator=(const SyncConfigPost &) = delete;

	void Start();
	void Cancel();

private:
	void OnReceipt(std::optional<MsgId> messageId);
	void OnReadback(SyncCandidateReadResult result);
	void Finish(SyncConfigPostResult result);

	not_null<Main::Session*> _session;
	base::weak_ptr<Main::Session> _sessionGuard;
	QByteArray _staged;
	Fn<void(SyncConfigPostResult)> _finished;
	std::unique_ptr<SyncCandidateReader> _reader;
	MsgId _messageId;
	bool _started = false;
	bool _posted = false;
	bool _done = false;
};

}

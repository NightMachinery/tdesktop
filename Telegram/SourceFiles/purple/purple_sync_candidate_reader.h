/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_sync_chat.h"
#include "purple/purple_sync_chat_io.h"
#include "purple/purple_account_config.h"

#include "base/weak_ptr.h"
#include "mtproto/sender.h"
#include "purple/purple_sync_inventory.h"

#include <memory>
#include <optional>
#include <vector>

class FileLoader;
namespace Main {
class Session;
}

namespace Purple {

class SyncCandidateReader final : public base::has_weak_ptr {
public:
	SyncCandidateReader(
		not_null<Main::Session*> session,
		std::vector<int32_t> candidateIds,
		Fn<void(SyncCandidateReadResult)> finished,
		Fn<SyncChatMessageOwnership(const MTPMessage &)> owned = {});
	SyncCandidateReader(
		not_null<Main::Session*> session,
		DesktopSyncChatIo route,
		std::vector<int32_t> candidateIds,
		Fn<void(SyncCandidateReadResult)> finished,
		Fn<bool()> current);
	~SyncCandidateReader();

	SyncCandidateReader(const SyncCandidateReader &) = delete;
	SyncCandidateReader &operator=(const SyncCandidateReader &) = delete;

	void Start();
	void Cancel();

private:
	[[nodiscard]] bool Current() const;
	void RequestNext();
	void StartDownload(const MTPDdocument &document);
	void OnDownload();
	void CompleteCurrent(SyncCandidateRecord record);
	void Finish(SyncCandidateReadStatus status);

	not_null<Main::Session*> _session;
	base::weak_ptr<Main::Session> _sessionGuard;
	std::optional<AccountOperationTarget> _opening;
	std::optional<DesktopSyncChatIo> _route;
	Fn<bool()> _current;
	MTP::Sender _api;
	std::vector<int32_t> _candidateIds;
	Fn<void(SyncCandidateReadResult)> _finished;
	SyncCandidateReadResult _result;
	std::optional<SyncCandidateRecord> _downloaded;
	std::unique_ptr<FileLoader> _loader;
	mtpRequestId _requestId = 0;
	size_t _next = 0;
	int64_t _expectedBytes = 0;
	uint64_t _documentId = 0;
	uint64_t _editDate = 0;
	Fn<SyncChatMessageOwnership(const MTPMessage &)> _owned;
	bool _downloadFailed = false;
	bool _started = false;
	bool _done = false;

};

} // namespace Purple

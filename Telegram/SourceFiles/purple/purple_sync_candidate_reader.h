/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/weak_ptr.h"
#include "mtproto/sender.h"
#include "purple/purple_sync_inventory.h"

#include <memory>
#include <optional>
#include <vector>

namespace Data {
class DocumentMedia;
}
class DocumentData;
namespace rpl {
class lifetime;
}
namespace Main {
class Session;
}

namespace Purple {

class SyncCandidateReader final : public base::has_weak_ptr {
public:
	SyncCandidateReader(
		not_null<Main::Session*> session,
		std::vector<int32_t> candidateIds,
		Fn<void(SyncCandidateReadResult)> finished);
	~SyncCandidateReader();

	SyncCandidateReader(const SyncCandidateReader &) = delete;
	SyncCandidateReader &operator=(const SyncCandidateReader &) = delete;

	void Start();
	void Cancel();

private:
	void RequestNext();
	void ResolveDocument(not_null<DocumentData*> document);
	void CheckDownload();
	void CompleteCurrent(SyncCandidateRecord record);
	void Finish(SyncCandidateReadStatus status);

	not_null<Main::Session*> _session;
	MTP::Sender _api;
	std::vector<int32_t> _candidateIds;
	Fn<void(SyncCandidateReadResult)> _finished;
	SyncCandidateReadResult _result;
	std::shared_ptr<Data::DocumentMedia> _media;
	std::unique_ptr<rpl::lifetime> _downloadLifetime;
	DocumentData *_document = nullptr;
	mtpRequestId _requestId = 0;
	size_t _next = 0;
	uint64_t _documentId = 0;
	uint64_t _editDate = 0;
	bool _started = false;
	bool _done = false;

};

} // namespace Purple

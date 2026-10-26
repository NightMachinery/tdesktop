#pragma once

#include "base/weak_ptr.h"
#include "purple/purple_sync_request_sender.h"
#include "purple/purple_sync_auto.h"
#include "purple/purple_sync_candidate_reader.h"

namespace Purple {

struct DesktopSyncExactMetadata {
	SyncAutoMessagePresence presence = SyncAutoMessagePresence::Unknown;
	bool owned = false;
	uint64_t documentId = 0;
	uint64_t editDate = 0;
};

[[nodiscard]] DesktopSyncExactMetadata InspectDesktopSyncExactMessage(
	const MTPmessages_Messages &result, int32_t id, uint64_t accountUserId);

[[nodiscard]] DesktopSyncExactMetadata InspectDesktopSyncExactMessage(
	const MTPmessages_Messages &result, int32_t id,
	const DesktopSyncChatIo &route);

class DesktopSyncExactRead final : public base::has_weak_ptr {
public:
	DesktopSyncExactRead(not_null<Main::Session*> session, int32_t id,
		Fn<bool()> current, Fn<void(SyncAutoServerRead)> finished);
	DesktopSyncExactRead(not_null<Main::Session*> session,
		DesktopSyncChatIo route, int32_t id,
		Fn<bool()> current, Fn<void(SyncAutoServerRead)> finished);
	void Start();

private:
	[[nodiscard]] bool Current() const;
	void Metadata(bool readback);
	void Finish(SyncAutoServerRead read);

	not_null<Main::Session*> _session;
	base::weak_ptr<Main::Session> _sessionGuard;
	std::optional<AccountOperationTarget> _opening;
	std::optional<DesktopSyncChatIo> _route;
	int32_t _id = 0;
	Fn<bool()> _current;
	Fn<void(SyncAutoServerRead)> _finished;
	SyncRequestSender _api;
	DesktopSyncExactMetadata _metadata;
	SyncCandidateRecord _record;
	std::unique_ptr<SyncCandidateReader> _reader;
	bool _done = false;

};

}

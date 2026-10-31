#pragma once

#include "purple/purple_sync_chat_carry_executor.h"
#include "purple/purple_sync_chat_cache.h"

#include <rpl/producer.h>

namespace Main { class Session; }

namespace Purple {

struct DesktopSyncCarryTransport {
	Fn<void(DesktopSyncCarryCommand, Fn<void(DesktopSyncCarryReply)>)> send;
	Fn<void()> cancel;
	Fn<void(const MTPUpdates &)> applyUpdates;
	Fn<bool(const DesktopSyncChatIo &, const MTPmessages_AffectedMessages &)> applyCleanup;
};

class DesktopSyncChatCarryOwner;
struct DesktopSyncChatCarryState;

// One live session's carry owner and its native host: a dedicated store thread, fresh
// resolution and peer facts, provision-only create, and revocation on logout, account
// switch, settings changes, lock and app deactivation. GUI thread only.
class DesktopSyncChatCarryService final {
public:
	explicit DesktopSyncChatCarryService(not_null<Main::Session*> session);
	~DesktopSyncChatCarryService();

	[[nodiscard]] DesktopSyncChatCarryOwner &owner();
	[[nodiscard]] rpl::producer<DesktopSyncChatCarryState> changes() const;
	// Resumes an unfinished carry journal, then runs `then` (ordinary reconciliation).
	void startup(Fn<void()> then);

private:
	class Impl;
	std::unique_ptr<Impl> _impl;

};

[[nodiscard]] DesktopSyncChatCarryService &SyncChatCarry(not_null<Main::Session*> session);
// With the Move, Follow/Fill and Recovery switches all OFF this only runs `then`.
void ResumeSyncChatCarry(not_null<Main::Session*> session, Fn<void()> then);

}

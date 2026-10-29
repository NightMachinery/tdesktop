#pragma once

#include "purple/purple_sync_chat_carry_executor.h"
#include "purple/purple_sync_chat_cache.h"

namespace Main { class Session; }

namespace Purple {

struct DesktopSyncCarryTransport {
	Fn<void(DesktopSyncCarryCommand, Fn<void(DesktopSyncCarryReply)>)> send;
	Fn<void()> cancel;
	Fn<void(const MTPUpdates &)> applyUpdates;
};

class DesktopSyncChatCarryService final {
public:
	DesktopSyncChatCarryService(not_null<Main::Session*> session,
		DesktopSyncChatDiscoveryScope opening, Fn<bool()> current,
		DesktopSyncCarryTransport transport, Fn<void(DesktopSyncCarryProgress)> progress);
	~DesktopSyncChatCarryService();
	void start(std::optional<SyncCarryJournalStart> begin = std::nullopt);
	void retry();
	void cancel();
	void discardInterrupted(bool confirmed);
	void skipCleanup(bool confirmed);
	void renew(const DesktopSyncCarryCommand &command, const DesktopSyncCarryPermit &permit,
		DesktopSyncCarryFresh fresh, Fn<void(std::optional<DesktopSyncCarryPermit>)> ready);

private:
	class Impl;
	std::unique_ptr<Impl> _impl;

};

}

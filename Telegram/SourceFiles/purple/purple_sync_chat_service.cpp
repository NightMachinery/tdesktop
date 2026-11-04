#include "purple/purple_sync_chat_service.h"
#include "purple/purple_sync_magic_service.h"
#include "core/application.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "crl/crl_on_main.h"
#include "rpl/lifetime.h"

namespace Purple {

bool SyncChatIoBelongsToSession(
		not_null<Main::Session*> session,
		const DesktopSyncChatIo &route) {
	return SyncMagicOwnsRoute(session, route);
}

std::optional<DesktopSyncChatLease> SyncChatIoForPost(
		not_null<Main::Session*> session,
		PeerToken chat) {
	return SyncMagicLease(session, chat);
}

void EnsureSyncChatResolved(not_null<Main::Session*> session, bool force) {
	SyncMagicLookup(
		session,
		force ? SyncMagicTrigger::Manual : SyncMagicTrigger::Screen,
		force);
}

void CheckSyncChatBeforePost(
	not_null<Main::Session*> session,
	Fn<void(DesktopSyncChatStatus)> done,
	DesktopSyncChatOperation operation) {
	const auto current = operation.current;
	SyncMagicLookup(session, SyncMagicTrigger::Post, true, [=](DesktopSyncChatStatus status) {
		if (current && !current()) {
			status.phase = DesktopSyncChatPhase::Paused;
			status.postingHeld = true;
			status.reason = u"caller-retired"_q;
		}
		done(status);
	});
}

DesktopSyncChatStatus SyncChatStatusOf(not_null<Main::Session*> session) {
	return SyncMagicStatusOf(session);
}

rpl::producer<Main::Session*> SyncChatStatusChanges() {
	return SyncMagicChanges();
}

void StartSyncChatService() {
	static auto started = false;
	if (std::exchange(started, true)) {
		return;
	}
	static auto lifetime = rpl::lifetime();
	static auto accounts = rpl::lifetime();
	const auto refresh = [] {
		accounts.destroy();
		for (const auto &entry : Core::App().domain().accounts()) {
			const auto ready = [](Main::Session* session) {
				if (session) {
					crl::on_main(session, [=] {
						SyncMagicLookup(session, SyncMagicTrigger::Startup, false);
					});
				}
			};
			entry.account->sessionChanges() | rpl::on_next(ready, accounts);
			if (const auto session = entry.account->maybeSession()) {
				ready(session);
			}
		}

	};
	Core::App().domain().accountsChanges() | rpl::on_next(refresh, lifetime);
	// WHY: app services start before the domain, and upstream adds stored accounts without
	// accountsChanges(), so an active session change is the first sign of a normal launch's sessions.
	Core::App().domain().activeSessionChanges() | rpl::on_next([=] { refresh(); }, lifetime);
	refresh();
}

}

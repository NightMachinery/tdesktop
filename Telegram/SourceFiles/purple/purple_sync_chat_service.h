#pragma once

#include "purple/purple_sync_chat_setup.h"
#include "purple/purple_sync_chat_recovery.h"
#include "purple/purple_sync_chat_consent.h"
#include "purple/purple_sync_chat_mark_actions.h"
#include "purple/purple_account_config.h"
#include <rpl/producer.h>

namespace Main {
class Session;
}

namespace Purple {

struct DesktopSyncChatOperation {
	QString directory;
	AccountKey account;
	Fn<bool()> current;
	std::optional<AccountRoutingSnapshot> routing;
	Fn<void(Fn<void(bool)>)> authorize;
};

struct DesktopSyncChatLease {
	DesktopSyncChatIo route;
	Fn<bool()> current;
};

[[nodiscard]] bool SyncChatIoBelongsToSession(
	not_null<Main::Session*> session, const DesktopSyncChatIo &route);
[[nodiscard]] std::optional<DesktopSyncChatLease> SyncChatIoForPost(
	not_null<Main::Session*> session, PeerToken chat);

[[nodiscard]] std::optional<DesktopSyncChatSetupSnapshot> SyncChatSetupSnapshotOf(
	not_null<Main::Session*> session);

[[nodiscard]] bool SyncChatSetupContextCurrent(
	not_null<Main::Session*> session, const DesktopSyncChatDiscoveryScope &scope);
[[nodiscard]] bool CachedSyncChatSetupContextCurrent(
	not_null<Main::Session*> session, const DesktopSyncChatDiscoveryScope &scope);

[[nodiscard]] auto ReviewSyncChatFollowFill(
	not_null<Main::Session*> session, DesktopSyncChatConsentAction &action,
	const DesktopSyncChatFollowOffer &offer, const DesktopSyncChatPeer &peer,
	DesktopSyncChatConsentChoice choice, bool disclosed)
-> std::optional<DesktopSyncChatConsentSelection>;

[[nodiscard]] bool ReviewSyncChatMove(
	not_null<Main::Session*> session, DesktopSyncChatMoveAction &action,
	const DesktopSyncChatMoveOffer &offer, std::optional<DesktopSyncChatPeer> peer,
	bool cleanup, bool consent, bool removeOldMark = false,
	Fn<void(DesktopSyncChatMoveDraft)> start = nullptr);

[[nodiscard]] auto ReviewSyncChatRecovery(
	not_null<Main::Session*> session, const DesktopSyncChatSetupSnapshot &opening,
	DesktopSyncChatRecoveryChoice choice, std::optional<DesktopSyncChatPeer> peer,
	QString window, bool disclosed)
-> std::optional<DesktopSyncChatRecoverySelection>;

[[nodiscard]] std::vector<DesktopSyncChatStaleMark> SyncChatStaleMarksOf(not_null<Main::Session*> session);
[[nodiscard]] bool ReviewSyncChatStaleMark(not_null<Main::Session*> session,
	const DesktopSyncChatStaleMark &row, Fn<bool()> current, Fn<void(DesktopSyncChatStaleMark)> start);

[[nodiscard]] std::vector<PeerToken> SyncChatLoadedDialogs(
	not_null<Main::Session*> session);

void StartSyncChatDiscovery();
void QuiesceSyncChatPublishers(not_null<Main::Session*> session);
[[nodiscard]] rpl::producer<Main::Session*> SyncChatPublisherQuiescence();
[[nodiscard]] std::optional<DesktopSyncChatDiscoveryScope> CaptureSyncChatCarryScope(
	not_null<Main::Session*> session);
void EnsureSyncChatResolved(
	not_null<Main::Session*> session, bool force = false);
// Forces a fresh resolution, then answers with the session's setup snapshot.
void ResolveSyncChatSetup(
	not_null<Main::Session*> session,
	Fn<void(std::optional<DesktopSyncChatSetupSnapshot>)> done);
void CheckSyncChatBeforePost(
	not_null<Main::Session*> session,
	Fn<void(DesktopSyncChatStatus)> done,
	DesktopSyncChatOperation operation = {});
[[nodiscard]] DesktopSyncChatStatus
SyncChatStatusOf(not_null<Main::Session*> session);
[[nodiscard]] bool SyncChatSelfResolved(not_null<Main::Session*> session);
[[nodiscard]] rpl::producer<Main::Session*> SyncChatStatusChanges();

}

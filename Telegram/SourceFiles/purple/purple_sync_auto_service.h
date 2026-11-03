#pragma once

#include "purple/purple_sync_auto.h"
#include "purple/purple_sync_config_flow.h"
#include "purple/purple_sync_chat_io.h"
#include "rpl/producer.h"

namespace Main { class Session; }

namespace Purple {

void StartAutomaticSettingsSync();
void TriggerAutomaticSettingsSync(SyncAutoEvent event);
void SuppressAutomaticSettingsSave(not_null<Main::Session*> session);
void ObserveAutomaticSettingsSave(not_null<Main::Session*> session);
// Moves this account's sync state into the bound chat; true once it is there.
bool MoveSyncStateToBoundChat(not_null<Main::Session*> session);
[[nodiscard]] bool SetAutomaticSettingsSync(not_null<Main::Session*> session, bool enabled);
[[nodiscard]] bool AutomaticSettingsSyncEnabled(not_null<Main::Session*> session);
[[nodiscard]] QString AutomaticSettingsSyncStatus(not_null<Main::Session*> session);
[[nodiscard]] rpl::producer<> AutomaticSettingsSyncChanges();
// Logs a NeedsReview refusal with its reason code; other statuses log nothing.
void NoteSyncConfigRefusal(const SyncConfigReview &review, bool manual);

enum class SyncFirstUseOutcome { Waiting, Asking, Published, Joined, Failed, NotNeeded };
struct SyncFirstUseResult {
	SyncFirstUseOutcome outcome = SyncFirstUseOutcome::Failed;
	QString text;
	QString reason;
};
// Decides on `inventory`, or on a fresh scan of the bound chat; `choice` answers an earlier Ask.
void RunSyncFirstUse(
	not_null<Main::Session*> session,
	std::optional<DesktopSyncChatInventory> inventory,
	SyncFirstUseChoice choice,
	Fn<void(SyncFirstUseResult)> done);
[[nodiscard]] std::optional<SyncFirstUseResult> SyncFirstUseResultOf(not_null<Main::Session*> session);

}

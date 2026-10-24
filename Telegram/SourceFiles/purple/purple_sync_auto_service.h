#pragma once

#include "purple/purple_sync_auto.h"
#include "rpl/producer.h"

namespace Main { class Session; }

namespace Purple {

void StartAutomaticSettingsSync();
void TriggerAutomaticSettingsSync(SyncAutoEvent event);
void SuppressAutomaticSettingsSave(not_null<Main::Session*> session);
void ObserveAutomaticSettingsSave(not_null<Main::Session*> session);
[[nodiscard]] bool SetAutomaticSettingsSync(not_null<Main::Session*> session, bool enabled);
[[nodiscard]] bool AutomaticSettingsSyncEnabled(not_null<Main::Session*> session);
[[nodiscard]] QString AutomaticSettingsSyncStatus(not_null<Main::Session*> session);
[[nodiscard]] rpl::producer<> AutomaticSettingsSyncChanges();

}

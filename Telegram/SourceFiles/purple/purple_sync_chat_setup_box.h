#pragma once

#include "purple/purple_sync_chat_carry_executor.h"

namespace Main { class Session; }
namespace Ui { class GenericBox; }

namespace Purple {

void SyncChatCleanupBox(not_null<Ui::GenericBox*> box, DesktopSyncCarryProgress opening,
	Fn<std::optional<DesktopSyncCarryProgress>()> latest, Fn<bool()> current, Fn<void()> retry, Fn<void(bool)> skip);

void SyncChatSetupBox(not_null<Ui::GenericBox*> box,
	not_null<Main::Session*> session, Fn<bool()> current);

}

#pragma once

#include "purple/purple_sync_magic_chat.h"

namespace Main { class Session; }
namespace Ui {
class GenericBox;
class Show;
} // namespace Ui

namespace Purple {

[[nodiscard]] QString SyncMagicProblemText(SyncChatProblem problem);
[[nodiscard]] QString SyncMagicStatusText(
	not_null<Main::Session*> session,
	const SyncMagicFrame &frame);
void ShowSyncMagicProblemOnce(
	not_null<Main::Session*> session,
	std::shared_ptr<Ui::Show> show);
// Shows a new problem once in the active window of that session, linking to the Sync chat box.
void ToastSyncMagicProblem(not_null<Main::Session*> session);

void SyncMagicChatBox(
	not_null<Ui::GenericBox*> box,
	not_null<Main::Session*> session,
	Fn<bool()> current);

} // namespace Purple

#pragma once

namespace Main { class Session; }
namespace Ui { class GenericBox; }

namespace Purple {

void SyncChatSetupBox(not_null<Ui::GenericBox*> box,
	not_null<Main::Session*> session, Fn<bool()> current);

}

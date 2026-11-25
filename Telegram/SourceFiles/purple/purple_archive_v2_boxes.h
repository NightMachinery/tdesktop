#pragma once

#include "data/data_types.h"
#include "purple/purple_peer_token.h"

namespace Window { class SessionController; }
namespace Ui { class PopupMenu; }

namespace Purple {

void AddArchiveHistoryAction(not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> window, FullMsgId item);
void OpenArchiveHistory(not_null<Window::SessionController*> window, FullMsgId item);
void OpenArchivedMessages(not_null<Window::SessionController*> window,
	std::optional<PeerToken> peer, bool deleted);
void OpenArchiveStorage(not_null<Window::SessionController*> window);
void ReviewArchiveReset(not_null<Window::SessionController*> window);

}

#pragma once

#include "purple/purple_sync_chat_facts.h"
#include <memory>

namespace Main { class Session; }

namespace Purple {

class DesktopSyncChatPeerRequest final {
public:
	DesktopSyncChatPeerRequest(
		not_null<Main::Session*> session,
		DesktopSyncChatOrigin origin,
		PeerToken chat,
		Fn<bool()> current,
		Fn<void(std::optional<DesktopSyncChatPeer>)> done);
	~DesktopSyncChatPeerRequest();

private:
	class Impl;
	std::unique_ptr<Impl> _impl;

};

}

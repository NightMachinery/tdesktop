#pragma once

#include "purple/purple_sync_chat_setup_flow.h"

namespace Main { class Session; }

namespace Purple {

class DesktopSyncChatSetupService final {
public:
	DesktopSyncChatSetupService(not_null<Main::Session*> session,
		DesktopSyncChatSetupSnapshot opening, DesktopSyncChatSetupSelection selection,
		Fn<bool()> current, Fn<void(DesktopSyncChatSetupProgress)> progress,
		bool resumeOnly = false);
	~DesktopSyncChatSetupService();
	void start();
	void retry();
	void useSelf();
	void reconsent(const DesktopSyncChatConsentOffer &offer);
	void cancel();

private:
	class Impl;
	std::unique_ptr<Impl> _impl;

};

}

#pragma once

#include "purple/purple_sync_chat_create_store.h"
#include "purple/purple_sync_chat_store.h"

namespace Purple {

struct DesktopSyncChatSetupProgress {
	DesktopSyncChatCreatePhase phase = DesktopSyncChatCreatePhase::Idle;
	QString reason;
	std::optional<DesktopSyncChatCreateJournal> journal;
	std::optional<DesktopSyncChatConsentOffer> consent;
	std::optional<DesktopSyncChatCreateCommand> command;
	bool resumeAbsent = false;
	bool provisioned = false;
};

class DesktopSyncChatSetupFlow final {
public:
	DesktopSyncChatSetupFlow(DesktopSyncChatSetupSnapshot opening,
		DesktopSyncChatSetupSelection selection, QString directory,
		QString statePath, std::shared_ptr<std::atomic<bool>> admitted,
		Fn<void(DesktopSyncChatSetupProgress)> progress, bool resumeOnly = false);
	void open(int64_t now);
	void resume(int64_t now);
	void useSelf(int64_t now);
	void reconsent(const DesktopSyncChatConsentOffer &offer, int64_t now);
	void complete(const DesktopSyncChatCreateReply &reply, int64_t now);
	void cancel();
	bool renew(const DesktopSyncChatCreateCommand &command,
		const DesktopSyncChatSetupResumeProof &proof, int64_t now);

private:
	void advance(int64_t now);
	void publish(QString error = {}, bool resumeAbsent = false);
	bool commit(const DesktopSyncChatCreateJournal &journal);

	bool _resumeOnly = false;
	DesktopSyncChatSetupSnapshot _opening;
	DesktopSyncChatSetupSelection _selection;
	QString _directory;
	QString _statePath;
	std::shared_ptr<std::atomic<bool>> _admitted;
	Fn<void(DesktopSyncChatSetupProgress)> _progress;
	std::unique_ptr<DesktopSyncChatCreateStore> _store;
	std::unique_ptr<DesktopSyncChatSetupExecutor> _executor;

};

}

#pragma once

#include "base/weak_ptr.h"
#include "purple/purple_sync_account_inventory.h"
#include "purple/purple_sync_chat_peer_request.h"
#include "purple/purple_sync_chat_setup_requests.h"
#include "purple/purple_sync_chat_service.h"
#include "purple/purple_sync_request_sender.h"

namespace Purple {

using DesktopSyncChatSetupRenewal = Fn<void(DesktopSyncChatCreateCommand,
	DesktopSyncChatSetupResumeProof, Fn<void(bool)>)>;

class DesktopSyncChatSetupTransport final : public base::has_weak_ptr {
public:
	DesktopSyncChatSetupTransport(not_null<Main::Session*> session,
		DesktopSyncChatDiscoveryScope scope, DesktopSyncChatOperation operation,
		Fn<bool()> current, DesktopSyncChatSetupRenewal renew);
	void send(DesktopSyncChatCreateCommand command,
		Fn<void(DesktopSyncChatCreateReply)> done);
	void cancel();

private:
	bool current(uint64_t generation) const;
	std::optional<DesktopSyncChatIo> route() const;
	void reply(DesktopSyncChatCreateReply reply, uint64_t generation);
	void fail(const QString &type, uint64_t generation);
	void mark(bool reconcile, uint64_t generation);
	void renewEffect(Fn<void(bool)> ready, uint64_t generation);
	void renewPeer(uint64_t generation, bool afterInventory);
	void renewMark(uint64_t generation);
	void approveRenewal(uint64_t generation);
	void finishRenewal(bool allowed, uint64_t generation);
	void readMark(int32_t id, bool reconcile, uint64_t generation);

	base::weak_ptr<Main::Session> _session;
	DesktopSyncChatDiscoveryScope _scope;
	DesktopSyncChatOperation _operation;
	Fn<bool()> _current;
	DesktopSyncChatSetupRenewal _renew;
	DesktopSyncChatSetupResumeProof _resumeProof;
	Fn<void(bool)> _resumeReady;
	SyncRequestSender _api;
	std::optional<DesktopSyncChatCreateCommand> _command;
	Fn<void(DesktopSyncChatCreateReply)> _done;
	std::optional<DesktopSyncChatPeer> _peer;
	std::optional<std::pair<QString, QByteArray>> _createdReceipt;
	std::unique_ptr<DesktopSyncChatPeerRequest> _peerRequest;
	std::unique_ptr<SyncAccountInventory> _inventory;
	uint64_t _generation = 0;

};

}

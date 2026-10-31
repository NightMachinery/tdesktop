#pragma once

#include "purple/purple_sync_chat_carry_service.h"
#include "purple/purple_sync_chat_mark_actions.h"

namespace Purple {

using DesktopSyncNativeRequest = std::variant<MTPmessages_SearchGlobal,
    MTPmessages_Search,
    MTPmessages_GetHistory,
    MTPmessages_GetMessages,
    MTPchannels_GetMessages,
    MTPmessages_ForwardMessages,
    MTPmessages_SendMessage>;

struct DesktopSyncNativeReply {
	std::optional<MTPmessages_Messages> messages;
	std::optional<MTPUpdates> updates;
	QString error;
};

using DesktopSyncCarryRenew
    = Fn<void(Fn<void(std::optional<DesktopSyncCarryPermit>)>)>;

struct DesktopSyncNativeIo {
	Fn<bool()> current;
	Fn<void(Fn<void()>)> onMain;
	Fn<void(int, Fn<void()>)> delay;
	Fn<std::optional<MTPInputPeer>(PeerToken)> input;
	Fn<void(PeerToken, Fn<void(std::optional<DesktopSyncChatIo>)>)> peer;
	Fn<void(DesktopSyncChatIo,
	    Fn<void(std::optional<DesktopSyncChatInventory>)>)>
	    inventory;
	Fn<void(DesktopSyncNativeRequest,
	    Fn<void(Fn<void(bool)>)>,
	    Fn<void(DesktopSyncNativeReply)>)>
	    request;
	Fn<void(DesktopSyncCarryPermit,
	    DesktopSyncCarryRenew,
	    Fn<void(std::optional<MTPUpdates>)>)>
	    upload;
	Fn<void(DesktopSyncCleanupPermit,
	    DesktopSyncCleanupRenew,
	    Fn<void(std::optional<MTPmessages_AffectedMessages>)>)>
	    cleanup;
	Fn<void(DesktopSyncChatMarkPermit,
	    DesktopSyncChatMarkRenewal,
	    Fn<void(std::optional<MTPmessages_AffectedMessages>)>)>
	    removeMark;
	Fn<void(const MTPUpdates &, uint64_t)> apply;
	Fn<bool(const DesktopSyncChatIo &, const MTPmessages_AffectedMessages &)>
	    affected;
	Fn<void(Fn<void(std::optional<QByteArray>)>)> localText;
	Fn<void()> cancel;
	std::vector<PeerToken> dialogs;
};

struct DesktopSyncNativeRenewal {
	Fn<void(DesktopSyncCarryCommand,
	    DesktopSyncCarryPermit,
	    DesktopSyncCarryFresh,
	    Fn<void(std::optional<DesktopSyncCarryPermit>)>)>
	    effect;
	Fn<void(DesktopSyncCarryCommand,
	    DesktopSyncCleanupPermit,
	    Fn<void(std::optional<DesktopSyncCleanupRead>)>)>
	    beginCleanup;
	Fn<void(DesktopSyncCarryCommand,
	    DesktopSyncCleanupPermit,
	    DesktopSyncCleanupRead,
	    DesktopSyncCarryReply,
	    Fn<void(std::optional<DesktopSyncCleanupPermit>)>)>
	    cleanup;
};

struct DesktopSyncNativeContext {
	DesktopSyncChatSetupSnapshot opening;
	DesktopSyncNativeRenewal renewal;
};

[[nodiscard]] DesktopSyncCarryTransport MakeDesktopSyncCarryTransport(
    not_null<Main::Session*> session,
    Fn<bool()> current,
    DesktopSyncNativeContext context);
[[nodiscard]] DesktopSyncChatMarkPort MakeDesktopSyncMarkNativePort(
    not_null<Main::Session*> session,
    Fn<bool()> current,
    DesktopSyncChatSetupSnapshot opening);

[[nodiscard]] DesktopSyncNativeIo MakeDesktopSyncNativeIo(
    not_null<Main::Session*> session,
    DesktopSyncChatSetupSnapshot opening,
    Fn<bool()> current);

void ResolveDesktopSyncNative(DesktopSyncChatSetupSnapshot opening,
    DesktopSyncNativeIo io,
    Fn<void(std::optional<DesktopSyncChatSetupSnapshot>)> done);

class DesktopSyncChatNative final {
public:
	DesktopSyncChatNative(DesktopSyncChatSetupSnapshot opening,
	    DesktopSyncNativeIo io,
	    DesktopSyncNativeRenewal renewal);
	~DesktopSyncChatNative();
	[[nodiscard]] DesktopSyncCarryTransport transport() const;
	[[nodiscard]] DesktopSyncChatMarkPort markPort(
	    DesktopSyncChatMarkPort context) const;
	void cancel();

private:
	class State;
	std::shared_ptr<State> _state;

};

}

#pragma once

#include "purple/purple_sync_chat_setup.h"
#include "purple/purple_sync_chat_create.h"
#include "purple/purple_sync_chat_carry_executor.h"

namespace Purple {

enum class DesktopSyncChatRecoveryChoice { Create, Choose, RepairMark };

struct DesktopSyncChatRecoverySelection {
	DesktopSyncChatSetupSnapshot opening;
	DesktopSyncChatRecoveryChoice choice = DesktopSyncChatRecoveryChoice::Choose;
	std::optional<DesktopSyncChatPeer> peer;
	QString window;
	QString fingerprint;
};

[[nodiscard]] bool DesktopSyncChatRecoveryActivated();
[[nodiscard]] QString DesktopSyncChatRecoveryHeld(const DesktopSyncChatSetupSnapshot &snapshot);
[[nodiscard]] QString DesktopSyncChatRepairHeld(
	const DesktopSyncChatSetupSnapshot &snapshot, const DesktopSyncChatPeer &peer);
[[nodiscard]] std::vector<DesktopSyncChatRecoveryChoice> DesktopSyncChatRecoveryChoices(
	const DesktopSyncChatSetupSnapshot &snapshot);
[[nodiscard]] auto SelectDesktopSyncChatRecovery(
	const DesktopSyncChatSetupSnapshot &snapshot, DesktopSyncChatRecoveryChoice choice,
	std::optional<DesktopSyncChatPeer> peer, QString window, bool disclosed)
-> std::optional<DesktopSyncChatRecoverySelection>;
[[nodiscard]] std::optional<DesktopSyncChatPeer> DesktopSyncChatRepairBaseline(
	const DesktopSyncChatSetupSnapshot &snapshot, const DesktopSyncChatPeer &peer);

struct DesktopSyncChatRecoveryPort {
	Fn<bool()> activated;
	Fn<bool()> current;
	Fn<bool()> foreground;
	Fn<DesktopSyncChatOrigin()> origin;
	Fn<QString()> login;
	Fn<QString()> window;
	Fn<void(Fn<void()>)> worker;
	Fn<void(Fn<void(std::optional<DesktopSyncChatSetupSnapshot>)>)> resolve;
	Fn<void(PeerToken, Fn<void(std::optional<DesktopSyncChatPeer>)>)> inspect;
	Fn<void(DesktopSyncChatMoveDraft, Fn<void(std::optional<DesktopSyncChatCreateJournal>, std::optional<DesktopSyncChatPeer>)>)> provision;
	Fn<void(Fn<void(std::optional<DesktopSyncChatCreateJournal>, std::optional<DesktopSyncChatPeer>)>)> resumeProvision;
	Fn<std::unique_ptr<DesktopSyncChatCarry>(std::shared_ptr<std::atomic<bool>>, Fn<void(DesktopSyncCarryProgress)>)> carry;
	Fn<void(DesktopSyncCarryCommand, Fn<void(DesktopSyncCarryReply)>)> send;
	Fn<void(DesktopSyncCarryProgress)> status;
	Fn<void()> cancel;
};

class DesktopSyncChatRecovery final {
public:
	DesktopSyncChatRecovery(DesktopSyncChatRecoverySelection selection, DesktopSyncChatRecoveryPort port);
	~DesktopSyncChatRecovery();
	void start();
	void retry();
	void close();

private:
	class State;
	std::shared_ptr<State> _state;

};

}

#include "purple/purple_sync_chat_create_requests.h"

#include <limits>

namespace Purple {

std::optional<DesktopSyncChatSetupWriteRequest> DesktopSyncChatSetupRequest(
		const DesktopSyncChatCreateCommand &command,
		const std::optional<DesktopSyncChatIo> &route) {
	const auto &j = command.journal;
	if (!command.ticket || command.operation != j.operation || !command.silent
		|| command.origin.account != j.account || !ValidDesktopSyncChatOrigin(command.origin)
		|| !j.pending || !SerializeDesktopSyncChatCreateJournal(j)) return {};
	if (command.step == DesktopSyncChatCreateStep::Create) {
		return !j.created && !j.chat && j.step == DesktopSyncChatCreateStep::Create
			&& j.createAttempts > 0 ? std::optional<DesktopSyncChatSetupWriteRequest>(DesktopSyncChatCreateRequest()) : std::nullopt;
	}
	if (!j.provisionSource.isEmpty()) { return {}; }
	if (!route || !ValidDesktopSyncChatIo(*route) || route->peer.origin != command.origin
		|| route->peer.chat != j.chat || DesktopSyncChatEligibility(route->peer)->readers.populationUnknown) return {};
	if (command.step == DesktopSyncChatCreateStep::PostMark && j.step == command.step) {
		return MTPmessages_SendMessage(MTP_flags(MTPmessages_SendMessage::Flag::f_silent),
			route->input, MTPInputReplyTo(), MTP_string(DesktopSyncChatMarkText()), MTP_long(j.randomId),
			MTPReplyMarkup(), MTP_vector<MTPMessageEntity>(), MTP_int(0), MTP_int(0),
			MTPInputPeer(), MTPInputQuickReplyShortcut(), MTP_long(0), MTP_long(0),
			MTPSuggestedPost(), MTPInputRichMessage());
	}
	if (!j.created || j.chat->kind != PeerTokenKind::Channel || j.markId <= 0
		|| j.markDate <= 0 || command.step != j.step) return {};
	if (command.step == DesktopSyncChatCreateStep::Pin) {
		return MTPmessages_UpdatePinnedMessage(
			MTP_flags(MTPmessages_UpdatePinnedMessage::Flag::f_silent), route->input, MTP_int(j.markId));
	} else if (command.step == DesktopSyncChatCreateStep::Mute) {
		return MTPaccount_UpdateNotifySettings(MTP_inputNotifyPeer(route->input),
			MTP_inputPeerNotifySettings(MTP_flags(MTPDinputPeerNotifySettings::Flag::f_mute_until),
				MTPBool(), MTPBool(), MTP_int(std::numeric_limits<int32_t>::max()),
				MTPNotificationSound(), MTPBool(), MTPBool(), MTPNotificationSound()));
	} else if (command.step == DesktopSyncChatCreateStep::Archive) {
		return MTPfolders_EditPeerFolders(MTP_vector<MTPInputFolderPeer>({
			MTP_inputFolderPeer(route->input, MTP_int(1)) }));
	}
	return {};
}

}

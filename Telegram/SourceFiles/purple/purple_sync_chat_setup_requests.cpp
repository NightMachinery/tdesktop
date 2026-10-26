#include "purple/purple_sync_chat_setup_requests.h"

#include <cstring>
#include <limits>

namespace Purple {

std::optional<MTPmessages_SendMessage> DesktopSyncChatMarkRequest(
		const DesktopSyncChatIo &route, uint64_t randomId) {
	if (!ValidDesktopSyncChatIo(route) || !randomId) {
		return {};
	}
	return MTPmessages_SendMessage(
		MTP_flags(MTPmessages_SendMessage::Flag::f_silent
			| MTPmessages_SendMessage::Flag::f_no_webpage),
		route.input, MTPInputReplyTo(), MTP_string(DesktopSyncChatMarkText()),
		MTP_long(randomId), MTPReplyMarkup(), MTP_vector<MTPMessageEntity>(),
		MTP_int(0), MTP_int(0), MTPInputPeer(), MTPInputQuickReplyShortcut(),
		MTP_long(0), MTP_long(0), MTPSuggestedPost(), MTPInputRichMessage());
}

std::optional<DesktopSyncChatDecoration> DesktopSyncChatDecorationRequest(
		const DesktopSyncChatCreateCommand &command,
		const DesktopSyncChatIo &route) {
	const auto &journal = command.journal;
	if (!ValidDesktopSyncChatIo(route) || !journal.created || !journal.chat
		|| *journal.chat != route.peer.chat || journal.markId <= 0
		|| route.peer.chat.kind != PeerTokenKind::Channel
		|| route.peer.origin != command.origin) {
		return {};
	}
	switch (command.step) {
	case DesktopSyncChatCreateStep::Pin:
		return MTPmessages_UpdatePinnedMessage(
			MTP_flags(MTPmessages_UpdatePinnedMessage::Flag::f_silent),
			route.input, MTP_int(journal.markId));
	case DesktopSyncChatCreateStep::Mute:
		return MTPaccount_UpdateNotifySettings(
			MTP_inputNotifyPeer(route.input), MTP_inputPeerNotifySettings(
				MTP_flags(MTPDinputPeerNotifySettings::Flag::f_mute_until
					| MTPDinputPeerNotifySettings::Flag::f_silent),
				MTPBool(), MTP_bool(true), MTP_int(std::numeric_limits<int32_t>::max()),
				MTPNotificationSound(), MTPBool(), MTPBool(), MTPNotificationSound()));
	case DesktopSyncChatCreateStep::Archive:
		return MTPfolders_EditPeerFolders(MTP_vector<MTPInputFolderPeer>(
			1, MTP_inputFolderPeer(route.input, MTP_int(1))));
	default:
		return {};
	}
}

QByteArray EncodeDesktopSyncChatSetupUpdates(const MTPUpdates &updates) {
	auto words = mtpBuffer();
	updates.write(words);
	if (words.empty() || words.size() > 256 * 1024 / sizeof(mtpPrime)) {
		return {};
	}
	return QByteArray(reinterpret_cast<const char*>(words.constData()),
		words.size() * sizeof(mtpPrime));
}

std::optional<MTPUpdates> DecodeDesktopSyncChatSetupUpdates(const QByteArray &bytes) {
	if (bytes.isEmpty() || bytes.size() > 256 * 1024 || bytes.size() % sizeof(mtpPrime)) {
		return {};
	}
	auto words = mtpBuffer(bytes.size() / sizeof(mtpPrime));
	std::memcpy(words.data(), bytes.constData(), bytes.size());
	const auto begin = words.constData();
	auto cursor = begin;
	auto updates = MTPUpdates();
	return updates.read(cursor, begin + words.size()) && cursor == begin + words.size()
		? std::optional(updates) : std::nullopt;
}

DesktopSyncChatCreateOutcome DesktopSyncChatSetupError(const QString &type) {
	if (type == u"CHANNELS_TOO_MUCH"_q || type == u"CHANNELS_ADMIN_PUBLIC_TOO_MUCH"_q) {
		return DesktopSyncChatCreateOutcome::Limit;
	}
	if (type == u"USER_RESTRICTED"_q || type == u"USER_BANNED_IN_CHANNEL"_q) {
		return DesktopSyncChatCreateOutcome::Restricted;
	}
	for (const auto &permanent : { u"CHAT_WRITE_FORBIDDEN"_q, u"CHANNEL_PRIVATE"_q,
			u"PEER_ID_INVALID"_q, u"USER_ID_INVALID"_q, u"CHAT_ADMIN_REQUIRED"_q,
			u"CHAT_RESTRICTED"_q, u"MESSAGE_TOO_LONG"_q, u"PURPLE_EFFECT_RESUME_REFUSED"_q }) {
		if (type == permanent) {
			return DesktopSyncChatCreateOutcome::Rejected;
		}
	}
	return DesktopSyncChatCreateOutcome::Ambiguous;
}

}

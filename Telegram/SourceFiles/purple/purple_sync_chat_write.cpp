#include "purple/purple_sync_chat_write.h"

namespace Purple {

bool CompatibleDesktopSyncChatIo(
		const DesktopSyncChatIo &opening,
		const DesktopSyncChatIo &fresh) {
	return ValidDesktopSyncChatIo(opening) && ValidDesktopSyncChatIo(fresh)
		&& opening.peer.origin == fresh.peer.origin
		&& opening.peer.chat == fresh.peer.chat
		&& opening.peer.kind == fresh.peer.kind
		&& !DesktopSyncChatEligibility(fresh.peer)->readers.populationUnknown;
}

std::optional<MTPmessages_SendMedia> DesktopSyncChatSendMediaRequest(
		const DesktopSyncChatIo &route,
		const MTPInputMedia &media,
		uint64_t randomId) {
	if (!ValidDesktopSyncChatIo(route) || !randomId) {
		return {};
	}
	return MTPmessages_SendMedia(
		MTP_flags(MTPmessages_SendMedia::Flag::f_silent), route.input,
		MTPInputReplyTo(), media, MTP_string("#purplesync"), MTP_long(randomId),
		MTPReplyMarkup(), MTP_vector<MTPMessageEntity>(), MTP_int(0), MTP_int(0),
		MTPInputPeer(), MTPInputQuickReplyShortcut(), MTP_long(0), MTP_long(0),
		MTPSuggestedPost());
}

std::optional<MTPmessages_EditMessage> DesktopSyncChatEditRequest(
		const DesktopSyncChatIo &route,
		const MTPInputMedia &media,
		int32_t id) {
	if (!ValidDesktopSyncChatIo(route) || id <= 0) {
		return {};
	}
	return MTPmessages_EditMessage(
		MTP_flags(MTPmessages_EditMessage::Flag::f_media
			| MTPmessages_EditMessage::Flag::f_message),
		route.input, MTP_int(id), MTP_string("#purplesync"), media,
		MTPReplyMarkup(), MTP_vector<MTPMessageEntity>(), MTP_int(0), MTP_int(0),
		MTP_int(0), MTPInputRichMessage());
}

std::optional<int32_t> DesktopSyncChatUploadReceipt(
		const DesktopSyncChatIo &route,
		const MTPUpdates &updates,
		uint64_t randomId,
		int32_t editId) {
	if (!ValidDesktopSyncChatIo(route) || (!editId && !randomId) || editId < 0) {
		return {};
	}
	auto mapped = std::optional<int32_t>();
	auto candidates = std::set<int32_t>();
	auto invalid = false;
	const auto message = [&](const MTPMessage &value, bool channel, bool edited) {
		if (channel != (route.peer.chat.kind == PeerTokenKind::Channel)
			|| edited != bool(editId) || value.type() != mtpc_message) {
			return;
		}
		const auto &data = value.c_message();
		if (data.vid().v <= 0 || data.vdate().v <= 0
			|| DesktopSyncChatToken(data.vpeer_id(), route.peer.origin.account.userId)
				!= route.peer.chat) {
			return;
		}
		if (GetSyncChatMessageOwnership(MapDesktopSyncChatMessage(
				route.peer.origin, route.peer, value)) != SyncChatMessageOwnership::Own) {
			invalid = true;
			return;
		}
		candidates.emplace(data.vid().v);
	};
	const auto update = [&](const MTPUpdate &value) {
		value.match([&](const MTPDupdateMessageID &data) {
			if (!editId && uint64_t(data.vrandom_id().v) == randomId) {
				const auto id = data.vid().v;
				if (id <= 0 || (mapped && *mapped != id)) invalid = true;
				else mapped = id;
			}
		}, [&](const MTPDupdateNewMessage &data) {
			message(data.vmessage(), false, false);
		}, [&](const MTPDupdateNewChannelMessage &data) {
			message(data.vmessage(), true, false);
		}, [&](const MTPDupdateEditMessage &data) {
			message(data.vmessage(), false, true);
		}, [&](const MTPDupdateEditChannelMessage &data) {
			message(data.vmessage(), true, true);
		}, [](const auto &) {});
	};
	const auto batch = [&](const auto &data) {
		for (const auto &value : data.vupdates().v) update(value);
	};
	updates.match([&](const MTPDupdates &data) {
		batch(data);
	}, [&](const MTPDupdatesCombined &data) {
		batch(data);
	}, [&](const MTPDupdateShort &data) {
		update(data.vupdate());
	}, [&](const MTPDupdateShortSentMessage &data) {
		if (!editId && route.peer.chat.kind != PeerTokenKind::Channel
			&& data.vid().v > 0 && data.vdate().v > 0 && !data.vttl_period()
			&& (route.peer.chat == PeerToken{} || data.is_out())) {
			mapped = data.vid().v;
			candidates.emplace(*mapped);
		}
	}, [](const auto &) {});
	if (invalid) {
		return {};
	}
	const auto expected = editId ? std::optional(editId) : mapped;
	return expected && candidates.contains(*expected) ? expected : std::nullopt;
}

}

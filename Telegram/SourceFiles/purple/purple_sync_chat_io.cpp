#include "purple/purple_sync_chat_io.h"

namespace Purple {

bool ValidDesktopSyncChatIo(const DesktopSyncChatIo &route) {
	const auto &peer = route.peer;
	if (!ValidDesktopSyncChatOrigin(peer.origin)
		|| !PeerTokenText(peer.chat, peer.origin.account.userId)
		|| !peer.facts || peer.facts->kind != peer.kind) {
		return false;
	}
	const auto token = *PeerTokenText(peer.chat, peer.origin.account.userId);
	if (ParsePeerToken(token, peer.origin.account.userId) != peer.chat) {
		return false;
	}
	const auto eligible = DesktopSyncChatEligibility(peer);
	if (!eligible || !*eligible) {
		return false;
	}
	const auto id = int64_t(peer.chat.id);
	return route.input.match([&](const MTPDinputPeerSelf &) {
		return peer.chat == PeerToken{} && peer.kind == SyncChatKind::Self;
	}, [&](const MTPDinputPeerUser &data) {
		return peer.chat.kind == PeerTokenKind::User
			&& (peer.kind == SyncChatKind::Private || peer.kind == SyncChatKind::Bot)
			&& data.vuser_id().v == id && data.vaccess_hash().v != 0;
	}, [&](const MTPDinputPeerChat &data) {
		return peer.chat.kind == PeerTokenKind::Chat
			&& peer.kind == SyncChatKind::BasicGroup && data.vchat_id().v == id;
	}, [&](const MTPDinputPeerChannel &data) {
		return peer.chat.kind == PeerTokenKind::Channel
			&& (peer.kind == SyncChatKind::Broadcast
				|| peer.kind == SyncChatKind::Supergroup)
			&& data.vchannel_id().v == id && data.vaccess_hash().v != 0;
	}, [](const auto &) {
		return false;
	});
}

std::optional<MTPInputChannel> DesktopSyncChatInputChannel(
		const DesktopSyncChatIo &route) {
	if (!ValidDesktopSyncChatIo(route)
		|| route.peer.chat.kind != PeerTokenKind::Channel) {
		return {};
	}
	const auto &data = route.input.c_inputPeerChannel();
	return MTP_inputChannel(data.vchannel_id(), data.vaccess_hash());
}

std::optional<MTPmessages_GetHistory> DesktopSyncChatHistoryRequest(
		const DesktopSyncChatIo &route,
		int offset) {
	if (!ValidDesktopSyncChatIo(route) || offset < 0) {
		return {};
	}
	return MTPmessages_GetHistory(route.input, MTP_int(offset), MTP_int(0),
		MTP_int(0), MTP_int(kSyncHistoryPageSize), MTP_int(0), MTP_int(0),
		MTP_long(0));
}

auto DesktopSyncChatRecordRequest(const DesktopSyncChatIo &route, int id)
-> std::optional<DesktopSyncChatExactRequest> {
	return ValidDesktopSyncChatIo(route)
		? DesktopSyncChatExactReadRequest(
			route.peer.chat, id, DesktopSyncChatInputChannel(route))
		: std::nullopt;
}

std::optional<MTPMessage> DesktopSyncChatRecordMessage(
		const DesktopSyncChatIo &route,
		const MTPmessages_Messages &response,
		int id) {
	if (!ValidDesktopSyncChatIo(route) || id <= 0
		|| !RecognizedDesktopSyncChatRead(response, route.peer.chat)) {
		return {};
	}
	const auto consistent = response.match(
		[](const MTPDmessages_messagesSlice &data) {
			return data.vcount().v >= data.vmessages().v.size();
		}, [](const MTPDmessages_channelMessages &data) {
			return data.vcount().v >= data.vmessages().v.size();
		}, [](const auto &) { return true; });
	if (!consistent) {
		return {};
	}
	const auto messages = response.match(
		[](const MTPDmessages_messagesNotModified &) {
			return static_cast<const QVector<MTPMessage>*>(nullptr);
		}, [](const auto &data) {
			return &data.vmessages().v;
		});
	if (!messages || messages->size() != 1) {
		return {};
	}
	const auto &message = messages->front();
	const auto valid = message.match([&](const MTPDmessage &data) {
		return data.vid().v == id && data.vdate().v > 0
			&& (!data.vedit_date() || data.vedit_date()->v >= 0)
			&& DesktopSyncChatToken(data.vpeer_id(), route.peer.origin.account.userId)
				== route.peer.chat;
	}, [&](const MTPDmessageService &data) {
		return data.vid().v == id && data.vdate().v > 0
			&& DesktopSyncChatToken(data.vpeer_id(), route.peer.origin.account.userId)
				== route.peer.chat;
	}, [&](const MTPDmessageEmpty &data) {
		if (data.vid().v != id) {
			return false;
		}
		if (const auto peer = data.vpeer_id()) {
			return DesktopSyncChatToken(*peer, route.peer.origin.account.userId)
				== route.peer.chat;
		}
		return false;
	});
	return valid ? std::optional(message) : std::nullopt;
}

std::optional<FullMsgId> DesktopSyncChatFileOrigin(
		const DesktopSyncChatIo &route,
		int id) {
	if (!ValidDesktopSyncChatIo(route) || id <= 0) {
		return {};
	}
	const auto chat = route.peer.chat;
	const auto peer = chat.kind == PeerTokenKind::Self
		? peerFromUser(UserId(route.peer.origin.account.userId))
		: chat.kind == PeerTokenKind::User ? peerFromUser(UserId(chat.id))
		: chat.kind == PeerTokenKind::Chat ? peerFromChat(ChatId(chat.id))
		: peerFromChannel(ChannelId(chat.id));
	return FullMsgId(peer, MsgId(id));
}

bool DesktopSyncChatInventoryMatches(
		const DesktopSyncChatInventory &inventory,
		const DesktopSyncChatOrigin &origin,
		PeerToken chat) {
	return ValidDesktopSyncChatIo(inventory.route)
		&& inventory.route.peer.origin == origin
		&& inventory.route.peer.chat == chat
		&& inventory.inventory.accountUserId == origin.account.userId;
}

}

#include "purple/purple_sync_chat_requests.h"

namespace Purple {

std::optional<DesktopSyncChatExactRequest> DesktopSyncChatExactReadRequest(
	PeerToken chat, int id, const std::optional<MTPInputChannel> &channel) {
	if (!PeerTokenText(chat) || id <= 0) {
		return {};
	}
	const auto ids =
		MTP_vector<MTPInputMessage>({MTP_inputMessageID(MTP_int(id))});
	if (chat.kind == PeerTokenKind::Channel) {
		if (!channel || channel->type() != mtpc_inputChannel
			|| channel->c_inputChannel().vchannel_id().v != int64_t(chat.id)) {
			return {};
		}
		return MTPchannels_GetMessages(*channel, ids);
	}
	return MTPmessages_GetMessages(ids);
}

bool RecognizedDesktopSyncChatRead(
	const MTPmessages_Messages &response, PeerToken chat) {
	if (!PeerTokenText(chat)) {
		return false;
	}
	return response.match(
		[](const MTPDmessages_messages &) {
			return true;
		},
		[](const MTPDmessages_messagesSlice &data) {
			return !data.is_inexact() && !data.vsearch_flood()
				&& data.vcount().v >= 0;
		},
		[=](const MTPDmessages_channelMessages &data) {
			return chat.kind == PeerTokenKind::Channel && !data.is_inexact()
				&& data.vcount().v >= 0;
		},
		[](const auto &) {
			return false;
		});
}

DesktopSyncChatHistoryGuard::DesktopSyncChatHistoryGuard(
		uint64_t selfUserId, PeerToken chat)
: _chat(chat)
, _selfUserId(selfUserId) {
}

DesktopSyncChatPageStatus DesktopSyncChatHistoryGuard::Add(
		const MTPmessages_Messages &response,
		uint64_t seen) {
	const auto refuse = [&] {
		_finished = true;
		return DesktopSyncChatPageStatus::Incomplete;
	};
	if (_finished || !_selfUserId || !PeerTokenText(_chat, _selfUserId)
		|| seen != _seen) {
		return refuse();
	}
	auto messages = static_cast<const QVector<MTPMessage>*>(nullptr);
	auto count = std::optional<int>();
	response.match([&](const MTPDmessages_messages &data) {
		messages = &data.vmessages().v;
	}, [&](const MTPDmessages_messagesSlice &data) {
		if (!data.is_inexact() && !data.vsearch_flood() && data.vcount().v >= 0) {
			messages = &data.vmessages().v;
			count = data.vcount().v;
		}
	}, [&](const MTPDmessages_channelMessages &data) {
		if (_chat.kind == PeerTokenKind::Channel && !data.is_inexact()
			&& data.vcount().v >= 0) {
			messages = &data.vmessages().v;
			count = data.vcount().v;
		}
	}, [](const auto &) {
	});
	if (!messages || messages->size() > 100) {
		return refuse();
	}
	for (const auto &message : *messages) {
		auto valid = false;
		message.match([&](const MTPDmessage &data) {
			valid = data.vid().v > 0 && data.vdate().v > 0
				&& DesktopSyncChatToken(data.vpeer_id(), _selfUserId) == _chat;
		}, [&](const MTPDmessageService &data) {
			valid = data.vid().v > 0 && data.vdate().v > 0
				&& DesktopSyncChatToken(data.vpeer_id(), _selfUserId) == _chat;
		}, [&](const MTPDmessageEmpty &data) {
			valid = data.vid().v > 0 && data.vpeer_id()
				&& DesktopSyncChatToken(*data.vpeer_id(), _selfUserId) == _chat;
		});
		if (!valid) {
			return refuse();
		}
	}
	if (count) {
		if (_total && *_total != uint64_t(*count)) {
			return refuse();
		}
		_total = uint64_t(*count);
	}
	const auto after = seen + uint64_t(messages->size());
	if (_total && (after > *_total || (messages->empty() && after != *_total))) {
		return refuse();
	}
	_seen = after;
	++_pages;
	if (_pages >= kDesktopSyncChatInventoryPages) {
		_finished = true;
		return DesktopSyncChatPageStatus::Capped;
	}
	if (messages->empty() || (_total && after == *_total)) {
		_finished = true;
		return DesktopSyncChatPageStatus::Complete;
	}
	return DesktopSyncChatPageStatus::Next;
}

}

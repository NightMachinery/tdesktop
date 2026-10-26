#include "purple/purple_sync_chat_requests.h"

namespace Purple {

MTPmessages_SearchGlobal DesktopSyncChatGlobalRequest(
	bool broadcastsOnly,
	const DesktopSyncChatCursor &cursor,
	const MTPInputPeer &offset) {
	return MTPmessages_SearchGlobal(
		MTP_flags(
			broadcastsOnly ? MTPmessages_SearchGlobal::Flag::f_broadcasts_only
						   : MTPmessages_SearchGlobal::Flag()),
		MTP_int(0),
		MTPInputChannel(),
		MTP_string("#purplechatsync"),
		MTP_inputMessagesFilterEmpty(),
		MTP_int(0),
		MTP_int(0),
		MTP_int(cursor.rate),
		cursor.peer ? offset : MTPInputPeer(MTP_inputPeerEmpty()),
		MTP_int(cursor.id),
		MTP_int(100));
}

MTPmessages_Search DesktopSyncChatSearchRequest(
	DesktopSyncChatSearch kind, const MTPInputPeer &peer, int offsetId) {
	const auto hint = kind == DesktopSyncChatSearch::SettingsHint;
	return MTPmessages_Search(
		MTP_flags(MTPmessages_Search::Flags()),
		peer,
		MTP_string(hint ? "#purplesync" : "#purplechatsync"),
		MTPInputPeer(),
		MTPInputPeer(),
		MTPVector<MTPReaction>(),
		MTP_int(0),
		hint ? MTP_inputMessagesFilterDocument()
			 : MTP_inputMessagesFilterEmpty(),
		MTP_int(0),
		MTP_int(0),
		MTP_int(offsetId),
		MTP_int(0),
		MTP_int(hint ? 1 : 100),
		MTP_int(0),
		MTP_int(0),
		MTP_long(0));
}

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

DesktopSyncChatSearchPage MapDesktopSyncChatSearchPage(
	const DesktopSyncChatOrigin &origin,
	const MTPmessages_Messages &response,
	DesktopSyncChatSearch kind,
	std::optional<PeerToken> target) {
	auto result = DesktopSyncChatSearchPage();
	if (!ValidDesktopSyncChatOrigin(origin)
		|| ((kind == DesktopSyncChatSearch::Self
			 || kind == DesktopSyncChatSearch::SettingsHint)
			&& (!target || *target != PeerToken{}))
		|| (kind == DesktopSyncChatSearch::Dialog && !target)) {
		return result;
	}
	const auto append = [&](const QVector<MTPMessage> &messages) {
		for (const auto &message : messages) {
			if (message.type() != mtpc_message) {
				result.recognized = false;
				return;
			}
			const auto &data = message.c_message();
			const auto peer =
				DesktopSyncChatToken(data.vpeer_id(), origin.account.userId);
			if (!peer || data.vid().v <= 0 || data.vdate().v <= 0
				|| (target && *target != *peer)) {
				result.recognized = false;
				return;
			}
			result.positions.push_back({*peer, data.vid().v, data.vdate().v});
		}
	};
	response.match(
		[&](const MTPDmessages_messages &data) {
			result.recognized = result.full = true;
			result.total = data.vmessages().v.size();
			append(data.vmessages().v);
		},
		[&](const MTPDmessages_messagesSlice &data) {
			result.recognized = !data.is_inexact() && !data.vsearch_flood();
			result.total = data.vcount().v;
			if (data.vnext_rate()) {
				result.nextRate = data.vnext_rate()->v;
			}
			append(data.vmessages().v);
		},
		[&](const MTPDmessages_channelMessages &data) {
			if (kind != DesktopSyncChatSearch::Dialog || !target
				|| target->kind != PeerTokenKind::Channel) {
				return;
			}
			result.recognized = !data.is_inexact();
			result.total = data.vcount().v;
			append(data.vmessages().v);
		},
		[&](const auto &) {
		});
	return result;
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

DesktopSyncChatSearchPages::DesktopSyncChatSearchPages(
	DesktopSyncChatOrigin origin, DesktopSyncChatSearch kind)
: _origin(std::move(origin))
, _kind(kind) {
}

DesktopSyncChatPageStatus
DesktopSyncChatSearchPages::Add(const DesktopSyncChatSearchPage &page) {
	if (_finished || !ValidDesktopSyncChatOrigin(_origin) || !page.recognized
		|| _kind == DesktopSyncChatSearch::SettingsHint || page.total < 0
		|| page.positions.size() > 100 || (_total && *_total != page.total)
		|| (page.nextRate && *page.nextRate < 0)) {
		_finished = true;
		return DesktopSyncChatPageStatus::Incomplete;
	}
	++_pages;
	_total = page.total;
	for (const auto &position : page.positions) {
		const auto peer = PeerTokenText(position.chat, _origin.account.userId);
		if (!peer || position.markId <= 0 || position.markDate <= 0) {
			_finished = true;
			return DesktopSyncChatPageStatus::Incomplete;
		}
		_seen.emplace(*peer, position.markId);
	}
	if (_seen.size() > size_t(page.total)
		|| (page.full && _seen.size() != size_t(page.total))) {
		_finished = true;
		return DesktopSyncChatPageStatus::Incomplete;
	} else if (_seen.size() == size_t(page.total)) {
		_finished = true;
		return DesktopSyncChatPageStatus::Complete;
	} else if (page.positions.empty()) {
		_finished = true;
		return DesktopSyncChatPageStatus::Incomplete;
	}
	const auto &last = page.positions.back();
	const auto cursor = DesktopSyncChatCursor{
		page.nextRate.value_or(0), last.chat, last.markId};
	const auto key = std::make_tuple(
		cursor.rate,
		*PeerTokenText(last.chat, _origin.account.userId),
		cursor.id);
	if (!_cursors.emplace(key).second
		|| (_cursor.id
			&& (_kind == DesktopSyncChatSearch::Self
				|| _kind == DesktopSyncChatSearch::Dialog)
			&& cursor.id >= _cursor.id)) {
		_finished = true;
		return DesktopSyncChatPageStatus::Incomplete;
	}
	_cursor = cursor;
	if (_pages >= 20) {
		_finished = true;
		return DesktopSyncChatPageStatus::Capped;
	}
	return DesktopSyncChatPageStatus::Next;
}

DesktopSyncChatCursor DesktopSyncChatSearchPages::cursor() const {
	return _cursor;
}

int DesktopSyncChatSearchPages::pages() const {
	return _pages;
}

}

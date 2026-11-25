#pragma once

#include "purple/purple_keys_mx.h"

#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Purple::ModalUnread {

enum class ChatBucket {
	DirectMessage,
	Group,
	Channel,
};

struct UnreadChatMetrics {
	int chats = 0;
	int chatsMuted = 0;
	int marks = 0;
	int marksMuted = 0;
	int messages = 0;
	int messagesMuted = 0;
	int mentions = 0;
	int reactions = 0;
	int reactionsMuted = 0;
	int polls = 0;
	int pollsMuted = 0;
	bool archived = false;
	bool hiddenByPreset = false;
	bool isSelf = false;
};

struct UnreadSummary {
	int dm = 0;
	int gp = 0;
	int ch = 0;

	[[nodiscard]] int total() const {
		return dm + gp + ch;
	}
	[[nodiscard]] bool operator==(const UnreadSummary &other) const = default;
};

[[nodiscard]] inline bool HasUnmutedUnreadNotification(
		const UnreadChatMetrics &m) {
	if (m.archived || m.hiddenByPreset || m.isSelf) {
		return false;
	}
	return (m.chats > m.chatsMuted)
		|| (m.marks > m.marksMuted)
		|| (m.mentions > 0)
		|| (m.reactions > m.reactionsMuted)
		|| (m.polls > m.pollsMuted);
}

[[nodiscard]] inline int UnmutedUnreadBadgeCount(const UnreadChatMetrics &m) {
	if (!HasUnmutedUnreadNotification(m)) {
		return 0;
	}
	const auto unmutedMsgs = std::max(0, m.messages - m.messagesMuted);
	if (unmutedMsgs > 0) {
		return unmutedMsgs;
	}
	if (m.mentions > 0) {
		return m.mentions;
	}
	return 1;
}

[[nodiscard]] inline QString BucketTag(ChatBucket bucket) {
	switch (bucket) {
	case ChatBucket::DirectMessage: return QStringLiteral("DM");
	case ChatBucket::Group: return QStringLiteral("GP");
	case ChatBucket::Channel: return QStringLiteral("CH");
	}
	return QStringLiteral("DM");
}

struct BufferItem {
	uint64_t peerId = 0;
	QString title;
	QString username;
	QString preview;
	ChatBucket bucket = ChatBucket::DirectMessage;
	bool isBot = false;
	bool isForum = false;
	int unreadCount = 0;
	int mentions = 0;
	bool markedUnread = false;
	int64_t lastTimestamp = 0;
};

[[nodiscard]] inline UnreadSummary SummarizeChats(
		const std::vector<std::pair<ChatBucket, UnreadChatMetrics>> &chats) {
	auto out = UnreadSummary();
	for (const auto &[bucket, metrics] : chats) {
		if (!HasUnmutedUnreadNotification(metrics)) {
			continue;
		}
		switch (bucket) {
		case ChatBucket::DirectMessage: ++out.dm; break;
		case ChatBucket::Group: ++out.gp; break;
		case ChatBucket::Channel: ++out.ch; break;
		}
	}
	return out;
}

[[nodiscard]] inline std::vector<int> FilterBufferItems(
		const std::vector<BufferItem> &items,
		const QString &query) {
	const auto tokens = ModalMx::ParseTokens(query);
	auto indices = std::vector<int>();
	indices.reserve(items.size());
	for (auto i = 0; i < int(items.size()); ++i) {
		const auto &it = items[i];
		if (tokens.empty()) {
			indices.push_back(i);
			continue;
		}
		auto hay = it.title;
		if (!it.username.isEmpty()) {
			hay += QLatin1String(" @") + it.username;
		}
		hay += QLatin1Char(' ') + BucketTag(it.bucket);
		if (it.isBot) {
			hay += QLatin1String(" bot");
		}
		if (it.isForum) {
			hay += QLatin1String(" forum topic");
		}
		if (!it.preview.isEmpty()) {
			hay += QLatin1Char(' ') + it.preview;
		}
		const auto hayFold = hay.toCaseFolded();
		const auto initials = ModalMx::WordInitials(it.title);
		const auto matched = std::all_of(
			tokens.begin(),
			tokens.end(),
			[&](const ModalMx::OrderlessToken &t) {
				return ModalMx::MatchToken(t, hayFold, initials);
			});
		if (matched) {
			indices.push_back(i);
		}
	}
	std::stable_sort(indices.begin(), indices.end(), [&](int a, int b) {
		if (items[a].lastTimestamp != items[b].lastTimestamp) {
			return items[a].lastTimestamp > items[b].lastTimestamp;
		}
		return QString::compare(items[a].title, items[b].title, Qt::CaseInsensitive) < 0;
	});
	return indices;
}

enum class MarkOp {
	MarkOne,
	UnmarkOne,
	ToggleOne,
	UnmarkAll,
	ToggleAllVisible,
	MarkBucketDM,
	MarkBucketGP,
	MarkBucketCH,
};

[[nodiscard]] inline uint64_t SelectedPeer(
		const std::vector<BufferItem> &items,
		const std::vector<int> &visible,
		int row) {
	return (row >= 0 && row < int(visible.size())) ? items[visible[row]].peerId : 0;
}

inline void ApplyMarkOp(
		QSet<uint64_t> &markedPeerIds,
		const std::vector<BufferItem> &items,
		const std::vector<int> &visibleIndices,
		int currentRow,
		MarkOp op) {
	const auto currentPeerId = [&] { return SelectedPeer(items, visibleIndices, currentRow); };
	switch (op) {
	case MarkOp::MarkOne:
		if (const auto id = currentPeerId()) {
			markedPeerIds.insert(id);
		}
		break;
	case MarkOp::UnmarkOne:
		if (const auto id = currentPeerId()) {
			markedPeerIds.remove(id);
		}
		break;
	case MarkOp::ToggleOne:
		if (const auto id = currentPeerId()) {
			if (markedPeerIds.contains(id)) {
				markedPeerIds.remove(id);
			} else {
				markedPeerIds.insert(id);
			}
		}
		break;
	case MarkOp::UnmarkAll:
		markedPeerIds.clear();
		break;
	case MarkOp::ToggleAllVisible:
		for (const auto idx : visibleIndices) {
			const auto id = items[idx].peerId;
			if (markedPeerIds.contains(id)) {
				markedPeerIds.remove(id);
			} else {
				markedPeerIds.insert(id);
			}
		}
		break;
	case MarkOp::MarkBucketDM:
	case MarkOp::MarkBucketGP:
	case MarkOp::MarkBucketCH: {
		const auto target = (op == MarkOp::MarkBucketDM)
			? ChatBucket::DirectMessage
			: (op == MarkOp::MarkBucketGP)
			? ChatBucket::Group
			: ChatBucket::Channel;
		for (const auto idx : visibleIndices) {
			if (items[idx].bucket == target) {
				markedPeerIds.insert(items[idx].peerId);
			}
		}
	} break;
	}
}

[[nodiscard]] inline bool NeedsMarkReadConfirmation(int targetCount) {
	return targetCount > 1;
}

} // namespace Purple::ModalUnread

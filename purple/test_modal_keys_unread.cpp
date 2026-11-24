#include "purple/purple_keys_unread.h"

#include <iostream>
#include <string>

namespace {

using namespace Purple::ModalUnread;

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const std::string &name) {
	++gChecks;
	if (!ok) {
		++gFailures;
		std::cerr << "FAIL: " << name << "\n";
	}
}

} // namespace

int main() {
	// 1. Unread & mute semantics: counts CHATS (not messages), respects Telegram mute/mention/archive/preset.
	{
		std::vector<std::pair<ChatBucket, UnreadChatMetrics>> chats = {
			// Unmuted DM with 12 messages -> counts as 1 DM chat.
			{ ChatBucket::DirectMessage, { .chats = 1, .messages = 12 } },
			// Muted DM with 5 messages, no mentions -> excluded.
			{ ChatBucket::DirectMessage, { .chats = 1, .chatsMuted = 1, .messages = 5, .messagesMuted = 5 } },
			// Muted DM with an unread @mention -> included as 1 DM chat.
			{ ChatBucket::DirectMessage, { .chats = 1, .chatsMuted = 1, .messages = 2, .messagesMuted = 2, .mentions = 1 } },
			// Unmuted bot chat (isUser -> DM) -> counts as 1 DM chat.
			{ ChatBucket::DirectMessage, { .chats = 1, .messages = 3 } },
			// Saved Messages (self) -> excluded even if counters non-zero.
			{ ChatBucket::DirectMessage, { .chats = 1, .messages = 1, .isSelf = true } },
			// Forum group with 4 unread unmuted topics -> AdjustedForumUnreadState has chats=1, still 1 GP chat!
			{ ChatBucket::Group, { .chats = 1, .messages = 4 } },
			// Manually marked unread unmuted group (0 messages, marks=1) -> counts as 1 GP chat.
			{ ChatBucket::Group, { .marks = 1 } },
			// Muted group with muted mark -> excluded.
			{ ChatBucket::Group, { .marks = 1, .marksMuted = 1 } },
			// Archived unmuted channel -> excluded.
			{ ChatBucket::Channel, { .chats = 1, .messages = 7, .archived = true } },
			// Work-preset hidden unmuted channel -> excluded.
			{ ChatBucket::Channel, { .chats = 1, .messages = 7, .hiddenByPreset = true } },
			// Unmuted channel -> counts as 1 CH chat.
			{ ChatBucket::Channel, { .chats = 1, .messages = 19 } },
		};
		const auto sum = SummarizeChats(chats);
		Check(sum.dm == 3, "DM count counts unmuted DMs, bots, and muted mentions (chats not messages)");
		Check(sum.gp == 2, "GP count counts forum group as 1 chat plus marked-unread group");
		Check(sum.ch == 1, "CH count excludes archived and preset-hidden channels");
		Check(sum.total() == 6, "total unread unmuted chats == 6");
	}

	// 2. Badge count helper.
	{
		Check(UnmutedUnreadBadgeCount({ .chats = 1, .messages = 9 }) == 9, "unmuted message badge count");
		Check(UnmutedUnreadBadgeCount({ .marks = 1 }) == 1, "marked unread falls back to 1");
		Check(UnmutedUnreadBadgeCount({ .chats = 1, .chatsMuted = 1, .messages = 4, .messagesMuted = 4, .mentions = 2 }) == 2,
			"muted chat with mentions uses mention count");
		Check(UnmutedUnreadBadgeCount({ .chats = 1, .chatsMuted = 1, .messages = 4, .messagesMuted = 4 }) == 0,
			"fully muted chat badge count is 0");
	}

	// 3. Orderless iBuffer filtering & sorting.
	{
		std::vector<BufferItem> items = {
			{ .peerId = 10, .title = QStringLiteral("Alice"), .username = QStringLiteral("alice"),
				.bucket = ChatBucket::DirectMessage, .unreadCount = 2, .lastTimestamp = 100 },
			{ .peerId = 20, .title = QStringLiteral("Rust Forum"), .bucket = ChatBucket::Group,
				.isForum = true, .unreadCount = 5, .lastTimestamp = 300 },
			{ .peerId = 30, .title = QStringLiteral("Release News"), .bucket = ChatBucket::Channel,
				.unreadCount = 1, .lastTimestamp = 200 },
			{ .peerId = 40, .title = QStringLiteral("Helper Bot"), .bucket = ChatBucket::DirectMessage,
				.isBot = true, .unreadCount = 1, .lastTimestamp = 250 },
		};
		const auto all = FilterBufferItems(items, QString());
		Check(all.size() == 4 && all[0] == 1 && all[1] == 3 && all[2] == 2 && all[3] == 0,
			"empty filter sorts newest first");

		const auto bots = FilterBufferItems(items, QStringLiteral("dm bot"));
		Check(bots.size() == 1 && items[bots[0]].peerId == 40, "orderless bucket + bot filter");

		const auto forum = FilterBufferItems(items, QStringLiteral("topic rust"));
		Check(forum.size() == 1 && items[forum[0]].peerId == 20, "orderless forum topic filter");

		const auto byHandle = FilterBufferItems(items, QStringLiteral("@alice"));
		Check(byHandle.size() == 1 && items[byHandle[0]].peerId == 10, "username handle filter");

		// 4. Mark operations.
		QSet<uint64_t> marks;
		ApplyMarkOp(marks, items, all, 0, MarkOp::MarkOne);
		Check(marks.contains(20) && marks.size() == 1, "mark one");
		ApplyMarkOp(marks, items, all, 0, MarkOp::ToggleOne);
		Check(marks.isEmpty(), "toggle one unmarks");
		ApplyMarkOp(marks, items, all, -1, MarkOp::MarkBucketDM);
		Check(marks.size() == 2 && marks.contains(10) && marks.contains(40), "mark bucket DM");
		ApplyMarkOp(marks, items, all, -1, MarkOp::ToggleAllVisible);
		Check(marks.size() == 2 && marks.contains(20) && marks.contains(30), "toggle all visible flips marks");
		ApplyMarkOp(marks, items, all, -1, MarkOp::UnmarkAll);
		Check(marks.isEmpty(), "unmark all clears");
	}

	// 5. Read-receipt confirmation threshold (required whenever marking more than 1 chat).
	{
		Check(!NeedsMarkReadConfirmation(0), "0 chats does not require confirmation");
		Check(!NeedsMarkReadConfirmation(1), "1 chat does not require confirmation");
		Check(NeedsMarkReadConfirmation(2), "2 chats requires confirmation");
		Check(NeedsMarkReadConfirmation(5), "5 chats requires confirmation");
	}

	std::cout << gChecks << " checks, " << gFailures << " failures\n";
	return gFailures ? 1 : 0;
}

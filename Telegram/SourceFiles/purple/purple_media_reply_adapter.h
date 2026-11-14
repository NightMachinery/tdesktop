#pragma once

class HistoryItem;
namespace Main { class Session; }
namespace Purple {
enum class MediaReplyStatusReason {
	None, JournalFull, JournalUnreadable, LockUnavailable, StorageRefused,
	Permission, Flood, ApprovalRequired, Uncertain, Cancelled,
};
struct MediaReplyStatus {
	MediaReplyStatusReason reason = MediaReplyStatusReason::None;
	int reserved = -1;
	int inFlight = 0;
	bool paused = false;
	bool disabled = true;
};
[[nodiscard]] MediaReplyStatus MediaReplyExecutionStatus(
	not_null<Main::Session*> session);
void StartMediaReplyServices();
void ObserveMediaReplyMessage(
	not_null<Main::Session*> session, const MTPMessage &message, bool replay);
void ObserveMediaReplyAck(not_null<HistoryItem*> item);
} // namespace Purple

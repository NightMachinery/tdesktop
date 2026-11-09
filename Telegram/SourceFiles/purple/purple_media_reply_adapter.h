#pragma once

class HistoryItem;
namespace Main { class Session; }
namespace Purple {
void StartMediaReplyServices();
void ObserveMediaReplyMessage(
	not_null<Main::Session*> session, const MTPMessage &message, bool replay);
void ObserveMediaReplyAck(not_null<HistoryItem*> item);
} // namespace Purple

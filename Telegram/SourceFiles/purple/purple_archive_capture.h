#pragma once

#include "data/data_types.h"

class HistoryItem;
struct TextForMimeData;
namespace Purple::ArchiveV2 { struct CapturedEvent; }
namespace Main {
class Session;
}

namespace Purple {

void CaptureArchiveBody(Main::Session *session, const MTPDmessage &data, ArchiveV2::CapturedEvent &event);
[[nodiscard]] TextForMimeData ArchivedText(const QByteArray &body);
void CancelArchiveDownloads(Main::Session *session);
void CancelArchiveAttachment(not_null<HistoryItem*> item);
void RecordArchiveExpiry(not_null<HistoryItem*> item, bool consumed);
void ObserveArchiveAttachment(not_null<HistoryItem*> item, bool explicitOpen);

void ObserveArchiveShortSent(not_null<Main::Session*> session,
	not_null<HistoryItem*> item, const MTPDupdateShortSentMessage &acknowledgement);
void ObserveArchiveSentMetadata(
	not_null<Main::Session*> session, const MTPDmessage &message);
void ObserveArchiveMessage(not_null<Main::Session*> session, const MTPMessage &message,
	MsgId id, MessageFlags flags, bool arrival = false);
void ObserveArchiveEdit(
	not_null<Main::Session*> session, const MTPMessage &message, int pts, bool channel);
void ObserveArchiveDelete(
	not_null<Main::Session*> session, PeerId peer, const QVector<MTPint> &ids, int pts);

}

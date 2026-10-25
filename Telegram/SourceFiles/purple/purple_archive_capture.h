#pragma once

#include "data/data_types.h"
#include "purple/purple_archive_model.h"

class HistoryItem;
namespace Main {
class Session;
}

namespace Purple {

[[nodiscard]] std::optional<ArchiveEvent> DesktopArchiveLiveSnapshot(
	not_null<Main::Session*> session, not_null<HistoryItem*> item);
[[nodiscard]] ArchiveChatKey DesktopArchiveChat(PeerId peer);
[[nodiscard]] std::optional<ArchiveEvent> DesktopArchiveSnapshot(
	const AccountKey &account, const MTPMessage &message, ArchiveOrigin origin,
	QString id, int64 observedAt);
[[nodiscard]] std::optional<ArchiveEvent> DesktopArchiveSnapshotData(
	const AccountKey &account, const MTPDmessage &data, ArchiveOrigin origin, QString id,
	int64 observedAt);
[[nodiscard]] std::optional<ArchiveEvent> DesktopArchiveShortSentSnapshot(
	ArchiveEvent event, const MTPDupdateShortSentMessage &acknowledgement);
void ObserveArchiveShortSent(not_null<Main::Session*> session,
	not_null<HistoryItem*> item, const MTPDupdateShortSentMessage &acknowledgement);
void ObserveArchiveSentMetadata(
	not_null<Main::Session*> session, const MTPDmessage &message);
[[nodiscard]] bool DesktopArchiveReceivedId(
	MsgId id, MessageFlags flags, const MTPMessage &message);
void ObserveArchiveMessage(not_null<Main::Session*> session, const MTPMessage &message,
	MsgId id, MessageFlags flags);
void ObserveArchiveEdit(
	not_null<Main::Session*> session, const MTPMessage &message, int pts, bool channel);
void ObserveArchiveDelete(
	not_null<Main::Session*> session, PeerId peer, const QVector<MTPint> &ids, int pts);

}

#include "purple/purple_archive_lifecycle_capture.h"

#include "base/unixtime.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "purple/purple_archive_capture.h"
#include "purple/purple_archive_v2_service.h"

namespace Purple {
namespace {

bool Expired(const MTPMessageMedia *media) {
	if (!media) {
		return false;
	}
	if (media->type() == mtpc_messageMediaPhoto) {
		const auto &data = media->c_messageMediaPhoto();
		return data.vttl_seconds() && !data.vphoto();
	}
	if (media->type() == mtpc_messageMediaDocument) {
		const auto &data = media->c_messageMediaDocument();
		return data.vttl_seconds() && !data.vdocument();
	}
	return false;
}

}

void ObserveArchiveLifecycle(not_null<HistoryItem*> item, ArchiveMediaTransition transition, bool wholeMessage) {
	const auto media = item->media();
	if (!IsServerMsgId(item->id) || item->isScheduled() || item->shortcutId()
		|| (!wholeMessage && (!media || !media->ttlSeconds()))) return;
	RecordArchiveExpiry(item, transition == ArchiveMediaTransition::Consumption);
}

void ObserveArchiveTtlExpiry(const std::vector<not_null<HistoryItem*>> &items) {
	for (const auto &item : items) ObserveArchiveLifecycle(item, ArchiveMediaTransition::Expiry, true);
}

void ObserveArchiveMediaReplacement(not_null<HistoryItem*> item, const MTPMessageMedia *replacement) {
	if (Expired(replacement)) ObserveArchiveLifecycle(item, ArchiveMediaTransition::ExpiredReplacement);
}

bool ObserveArchiveExpiredReplacement(not_null<Main::Session*> session, const MTPMessage &message, int pts) {
	if (message.type() != mtpc_message || !Expired(message.c_message().vmedia())) return false;
	const auto &data = message.c_message();
	const auto peer = peerFromMTP(data.vpeer_id());
	const auto id = MsgId(data.vid().v);
	if (const auto item = session->data().message(peer, id)) {
		ObserveArchiveMediaReplacement(item, data.vmedia());
	} else if (IsServerMsgId(id)) {
		(void)ArchiveV2::Queue(session, {
			.identity = { ArchiveV2::Peer(peer), uint64(id.bare) },
			.event = ArchiveV2::Observation::Expire,
			.eventId = "expired/" + QByteArray::number(pts),
			.observedAt = base::unixtime::now(),
		});
	}
	return true;
}

}

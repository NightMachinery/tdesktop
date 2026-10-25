#include "purple/purple_archive_lifecycle_capture.h"

#include "base/unixtime.h"
#include "data/data_media_types.h"
#include "data/data_document.h"
#include "data/data_photo.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "purple/purple_archive_acquired_source.h"
#include "purple/purple_archive_capture.h"
#include "purple/purple_archive_service.h"
#include "purple/purple_config.h"
#include <QtCore/QCryptographicHash>
#include <map>
#include <set>

namespace Purple {
namespace {

std::map<Main::Session*, std::set<QString>> Submitted;

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

QString Identity(not_null<HistoryItem*> item) {
	const auto media = item->media();
	if (const auto document = media ? media->document() : nullptr) {
		return u"document:"_q + QString::number(document->id);
	}
	if (const auto photo = media ? media->photo() : nullptr) {
		return u"photo:"_q + QString::number(photo->id);
	}
	return {};
}

}

void ObserveArchiveLifecycle(not_null<HistoryItem*> item, ArchiveMediaTransition transition, bool wholeMessage) {
	const auto media = item->media();
	if (!IsServerMsgId(item->id) || item->isScheduled() || item->shortcutId()
		|| (!wholeMessage && (!media || !media->ttlSeconds()))) {
		return;
	}
	const auto session = &item->history()->session();
	const auto identity = Identity(item);
	const auto key = ArchiveMessageKey{SessionAccountKey(session), DesktopArchiveChat(item->history()->peer->id), item->id.bare};
	const auto observed = ArchiveAcquisitionObservation(session, key, identity);
	auto snapshot = DesktopArchiveLiveSnapshot(session, item);
	if (!snapshot) {
		QueueArchiveRetentionGap(session, u"lifecycle-preimage-unavailable"_q);
		return;
	}
	const auto token = QByteArray::number(int(key.chat.kind)) + '/' + QByteArray::number(key.chat.id)
		+ '/' + QByteArray::number(key.messageId) + '/' + (observed ? observed->observation.toUtf8() : QByteArray("live"));
	snapshot->id = QString::fromLatin1(QCryptographicHash::hash(token, QCryptographicHash::Sha256).toHex());
	const auto [i, inserted] = Submitted.try_emplace(session);
	if (inserted) {
		session->lifetime().add([session] { Submitted.erase(session); });
	}
	if (!i->second.insert(snapshot->id).second) {
		return;
	}
	if (i->second.size() > 4096) {
		i->second.erase(i->second.begin());
	}
	snapshot->observation.context.insert(u"desktopLivePreimage"_q, true);
	snapshot->observation.context.insert(u"desktopLifecycle"_q, int(transition));
	snapshot->observation.context.insert(u"desktopWholeMessageTtl"_q, wholeMessage);
	snapshot->observation.context.insert(u"desktopViewOnce"_q, media && media->ttlSeconds() == 0x7fffffff);
	if (observed) {
		snapshot->observation.context.insert(u"desktopLifecycleObservation"_q, observed->observation);
		snapshot->observation.context.insert(u"desktopLifecycleHash"_q, observed->contentHash);
	}
	QueueArchiveLifecycleFact(session, std::move(*snapshot));
}

void ObserveArchiveTtlExpiry(const std::vector<not_null<HistoryItem*>> &items) {
	for (const auto &item : items) {
		ObserveArchiveLifecycle(item, ArchiveMediaTransition::Expiry, true);
	}
}

void ObserveArchiveMediaReplacement(not_null<HistoryItem*> item, const MTPMessageMedia *replacement) {
	if (Expired(replacement)) {
		ObserveArchiveLifecycle(item, ArchiveMediaTransition::ExpiredReplacement);
	}
}

bool ObserveArchiveExpiredReplacement(not_null<Main::Session*> session, const MTPMessage &message, int pts) {
	if (message.type() != mtpc_message || !Expired(message.c_message().vmedia())) {
		return false;
	}
	const auto &data = message.c_message();
	if (const auto item = session->data().message(peerFromMTP(data.vpeer_id()), data.vid().v)) {
		ObserveArchiveLifecycle(item, ArchiveMediaTransition::ExpiredReplacement);
	} else {
		auto event = ArchiveEvent();
		event.key = {SessionAccountKey(session), DesktopArchiveChat(peerFromMTP(data.vpeer_id())), data.vid().v};
		event.id = u"expired:"_q + QString::number(pts) + ':' + QString::number(data.vid().v);
		event.content = ArchiveContent();
		event.observation.observedAt = base::unixtime::now();
		event.observation.context.insert(u"desktopLifecycleFallback"_q, true);
		event.observation.context.insert(u"desktopLifecycle"_q, int(ArchiveMediaTransition::ExpiredReplacement));
		event.observation.context.insert(u"desktopLivePreimage"_q, true);
		QueueArchiveLifecycleFact(session, std::move(event));
	}
	return true;
}

}

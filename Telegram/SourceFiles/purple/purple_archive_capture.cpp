#include "purple/purple_archive_capture.h"
#include "api/api_text_entities.h"
#include "base/unixtime.h"
#include "core/file_location.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "iv/iv_rich_page.h"
#include "main/main_session.h"
#include "purple/purple_archive_crypto.h"
#include "purple/purple_archive_files.h"
#include "purple/purple_archive_service.h"
#include "purple/purple_article_markdown.h"
#include "purple/purple_config.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonArray>

namespace Purple {
namespace {

QString Fresh() {
	const auto bytes = DesktopArchiveCrypto::Random(16);
	return bytes ? QString::fromLatin1(bytes->toHex()) : QString();
}
QString Digest(const QByteArray &bytes) {
	return QString::fromLatin1(
		QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QJsonArray Entities(const EntitiesInText &entities) {
	auto result = QJsonArray();
	for (const auto &entity : entities) {
		result.push_back(
			QJsonObject{{u"type"_q, int(entity.type())}, {u"offset"_q, entity.offset()},
				{u"length"_q, entity.length()}, {u"data"_q, entity.data()}});
	}
	return result;
}

void Available(not_null<Main::Session*> session, const ArchiveEvent &event) {
	if (!event.content) {
		return;
	}
	for (const auto &descriptor : event.content->media) {
		auto ok = false;
		const auto id = descriptor.identity.mid(descriptor.identity.indexOf(':') + 1)
							.toULongLong(&ok);
		if (!ok || !id) {
			continue;
		}
		if (descriptor.kind == u"document"_q
			&& descriptor.identity.startsWith(u"document:"_q)) {
			const auto document = session->data().document(id);
			if (document->size < 0) {
				continue;
			}
			const auto media = document->activeMediaView();
			const auto bytes = media ? media->bytes() : QByteArray();
			const auto path = document->filepath(false);
			if ((!bytes.isEmpty() || (media && media->loaded() && !document->size))
				|| !path.isEmpty()) {
				const auto proof
					= !path.isEmpty() && document->location(false).check()
						  ? DesktopArchiveInspectSource(path, uint64_t(document->size))
						  : std::nullopt;
				QueueArchiveAvailableMedia(session, event.key, event.id,
					ArchiveContentHash(*event.content), descriptor.identity, bytes, path,
					uint64_t(document->size), proof);
			}
		} else if (descriptor.kind == u"photo"_q
				   && descriptor.identity.startsWith(u"photo:"_q)) {
			const auto photo = session->data().photo(id);
			const auto media = photo->activeMediaView();
			const auto bytes
				= media ? media->imageBytes(Data::PhotoSize::Large) : QByteArray();
			if (!bytes.isEmpty()) {
				QueueArchiveAvailableMedia(session, event.key, event.id,
					ArchiveContentHash(*event.content), descriptor.identity, bytes, {},
					uint64_t(bytes.size()));
			}
		}
	}
}

std::optional<ArchiveEvent> LiveSnapshot(
	not_null<Main::Session*> session, not_null<HistoryItem*> item) {
	if (!IsServerMsgId(item->id) || !item->isRegular() || item->isScheduled()
		|| item->shortcutId()) {
		return {};
	}
	auto event = ArchiveEvent();
	event.key = {SessionAccountKey(session),
		DesktopArchiveChat(item->history()->peer->id), item->id.bare};
	event.id = Fresh();
	event.content = ArchiveContent();
	event.content->text = item->originalText().text;
	event.content->entities = Entities(item->originalText().entities);
	event.observation.observedAt = base::unixtime::now();
	event.observation.serverTime = item->date();
	if (const auto page
		= item->fullRichPage() ? item->fullRichPage() : item->richPage()) {
		const auto rendered = RenderArticleMarkdown(*page);
		event.content->rich = QJsonObject{
			{u"format"_q, u"observed-article-markdown"_q}, {u"text"_q, rendered.text}};
	}
	if (const auto media = item->media()) {
		if (const auto document = media->document()) {
			event.content->media.push_back(
				{u"document:"_q + QString::number(document->id), u"document"_q});
		}
		if (const auto photo = media->photo()) {
			event.content->media.push_back(
				{u"photo:"_q + QString::number(photo->id), u"photo"_q});
		}
	}
	event.completeness = ArchiveCompleteness::Partial;
	event.missing.content.push_back({{u"observedStructure"_q}});
	return event;
}

std::optional<ArchiveEvent> Preimage(
	not_null<Main::Session*> session, const MTPMessage &message) {
	if (message.type() != mtpc_message) {
		return {};
	}
	const auto &data = message.c_message();
	const auto item = session->data().message(peerFromMTP(data.vpeer_id()), data.vid().v);
	if (!item || !item->isRegular() || item->isScheduled()) {
		return {};
	}
	auto event = LiveSnapshot(session, item);
	if (event) {
		event->observation.context.insert(u"desktopLivePreimage"_q, true);
	}
	return event;
}

}

void ObserveArchiveShortSent(not_null<Main::Session*> session,
	not_null<HistoryItem*> item, const MTPDupdateShortSentMessage &acknowledgement) {
	auto observed = LiveSnapshot(session, item);
	const auto event = observed
		? DesktopArchiveShortSentSnapshot(std::move(*observed), acknowledgement)
		: std::nullopt;
	if (event) {
		QueueArchiveObservation(session, *event);
		Available(session, *event);
	}
}

void ObserveArchiveSentMetadata(
	not_null<Main::Session*> session, const MTPDmessage &message) {
	auto event = DesktopArchiveSnapshotData(SessionAccountKey(session), message,
		ArchiveOrigin::Baseline, Fresh(), base::unixtime::now());
	if (!event) {
		return;
	}
	event->observation.context.insert(u"desktopSendMetadata"_q, true);
	QueueArchiveObservation(session, *event);
	Available(session, *event);
}

void ObserveArchiveMessage(not_null<Main::Session*> session, const MTPMessage &message,
	MsgId id, MessageFlags flags) {
	if (!DesktopArchiveReceivedId(id, flags, message)) {
		return;
	}
	if (const auto event = DesktopArchiveSnapshot(SessionAccountKey(session), message,
			ArchiveOrigin::Baseline, Fresh(), base::unixtime::now())) {
		QueueArchiveObservation(session, *event);
		Available(session, *event);
	}
}

void ObserveArchiveEdit(
	not_null<Main::Session*> session, const MTPMessage &message, int pts, bool channel) {
	if (const auto preimage = Preimage(session, message)) {
		QueueArchiveObservation(session, *preimage);
		Available(session, *preimage);
	}
	auto event = DesktopArchiveSnapshot(SessionAccountKey(session), message,
		ArchiveOrigin::AuthoritativeEdit, Fresh(), base::unixtime::now());
	if (!event) {
		return;
	}
	event->id
		= Digest((channel ? QByteArray("channel-edit/") : QByteArray("global-edit/"))
				 + QByteArray::number(pts) + '/' + QByteArray::number(event->key.chat.id)
				 + '/' + QByteArray::number(event->key.messageId) + '/'
				 + ArchiveContentHash(*event->content).toUtf8());
	event->observation.context.insert(u"acceptedPts"_q, pts);
	QueueArchiveObservation(session, *event);
	Available(session, *event);
}

void ObserveArchiveDelete(
	not_null<Main::Session*> session, PeerId peer, const QVector<MTPint> &ids, int pts) {
	for (const auto &id : ids) {
		if (id.v <= 0) {
			continue;
		}
		auto event = ArchiveEvent();
		event.key = {SessionAccountKey(session),
			peer ? DesktopArchiveChat(peer) : ArchiveChatKey{ArchiveChatKind::User, 1},
			id.v};
		event.id
			= Digest(QByteArray("delete/") + QByteArray::number(pts) + '/'
					 + QByteArray::number(peer.value) + '/' + QByteArray::number(id.v));
		event.origin = ArchiveOrigin::AuthoritativeDelete;
		event.observation.observedAt = base::unixtime::now();
		event.observation.context.insert(u"desktopNonChannelDelete"_q, !peer);
		QueueArchiveObservation(session, std::move(event));
	}
}

}

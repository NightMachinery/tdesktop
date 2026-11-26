#include "purple/purple_archive_capture.h"

#include "api/api_text_entities.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "iv/iv_rich_page.h"
#include "main/main_session.h"
#include "purple/purple_archive_v2_service.h"
#include "purple/purple_archive_lifecycle_capture.h"
#include "purple/purple_markdown_current.h"
#include "ui/text/text_entity.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>

namespace Purple {
namespace {

using namespace ArchiveV2;

QByteArray Pack(const TextWithEntities &text, PeerId sender,
		const std::vector<Attachment> &attachments = {}) {
	auto entities = QJsonArray();
	for (const auto &entity : text.entities) {
		entities.push_back(QJsonArray{ int(entity.type()), entity.offset(), entity.length(), entity.data() });
	}
	auto media = QJsonArray();
	for (const auto &entry : attachments) media.push_back(QString::fromLatin1(entry.source));
	return QJsonDocument(QJsonObject{ { u"text"_q, text.text }, { u"entities"_q, entities },
		{ u"sender"_q, QString::number(sender.value) }, { u"media"_q, media } }).toJson(QJsonDocument::Compact);
}

void Attach(const MTPMessageMedia *media, CapturedEvent &event) {
	if (!media) return;
	if (media->type() == mtpc_messageMediaPhoto) {
		const auto &data = media->c_messageMediaPhoto();
		if (const auto photo = data.vphoto(); photo && photo->type() == mtpc_photo) {
			event.attachments.push_back({ "photo:" + QByteArray::number(photo->c_photo().vid().v),
				"Photo", int(MediaKind::Photo), !!data.vttl_seconds() });
		}
	} else if (media->type() == mtpc_messageMediaDocument) {
		const auto &data = media->c_messageMediaDocument();
		const auto document = data.vdocument();
		if (!document || document->type() != mtpc_document) return;
		const auto &value = document->c_document();
		auto kind = MediaKind::Other;
		auto name = QString();
		for (const auto &attribute : value.vattributes().v) {
			switch (attribute.type()) {
			case mtpc_documentAttributeFilename: name = qs(attribute.c_documentAttributeFilename().vfile_name()); break;
			case mtpc_documentAttributeAudio:
				if (attribute.c_documentAttributeAudio().is_voice()) kind = MediaKind::Voice;
				break;
			case mtpc_documentAttributeVideo:
				kind = attribute.c_documentAttributeVideo().is_round_message() ? MediaKind::Round : MediaKind::Video;
				break;
			}
		}
		event.attachments.push_back({ "document:" + QByteArray::number(value.vid().v),
			QJsonDocument(QJsonObject{ { u"name"_q, name }, { u"mime"_q, qs(value.vmime_type()) },
				{ u"bytes"_q, QString::number(value.vsize().v) } }).toJson(QJsonDocument::Compact),
			int(kind), !!data.vttl_seconds() });
	}
}

CapturedEvent Event(PeerId peer, MsgId id, Observation kind, QByteArray token, PeerData *chat = nullptr) {
	return { .identity = { Peer(peer), uint64(id.bare) }, .kind = Classify(chat),
		.event = kind, .eventId = std::move(token), .observedAt = base::unixtime::now() };
}

QByteArray Preimage(HistoryItem *item) {
	return item && (item->isRegular() || item->isTtlCoveredMedia())
		? Pack(item->originalText(), item->from()->id) : QByteArray();
}

void Accept(not_null<Main::Session*> session, const MTPDmessage &data, Observation event,
		QByteArray token, bool arrival = false) {
	if (!IsServerMsgId(MsgId(data.vid().v)) || data.vquick_reply_shortcut_id() || !SessionLease(session.get())) return;
	const auto peer = peerFromMTP(data.vpeer_id());
	const auto msg = MsgId(data.vid().v);
	auto captured = Event(peer, msg, event, std::move(token), session->data().peerLoaded(peer));
	CaptureArchiveBody(session.get(), data, captured);
	if (event == Observation::Edit) captured.preimage = Preimage(session->data().message(peer, msg));
	const auto request = arrival && !captured.attachments.empty() && !captured.attachments.front().timed;
	if (Queue(session, std::move(captured)) && request) {
		const auto weak = base::make_weak(session.get());
		const auto lease = SessionLease(session.get());
		crl::on_main([=] {
			if (const auto live = weak.get(); live && SessionLease(live) == lease) {
				if (const auto item = live->data().message(peer, msg)) ObserveArchiveAttachment(item, false);
			}
		});
	}
}

}

void CaptureArchiveBody(Main::Session *session, const MTPDmessage &data, ArchiveV2::CapturedEvent &event) {
	Attach(data.vmedia(), event);
	const auto sender = data.vfrom_id() ? peerFromMTP(*data.vfrom_id()) : peerFromMTP(data.vpeer_id());
	// Rich messages carry their displayed body in the blocks, even when message is empty.
	const auto rich = data.vrich_message();
	auto text = rich ? Iv::FlattenRichPageSummary(Iv::ParseRichPage(session, *rich), false) : TextWithEntities();
	if (text.empty()) {
		text = { qs(data.vmessage()), Api::EntitiesFromMTP(session, data.ventities().value_or_empty()) };
	}
	event.body = Pack(text, sender, event.attachments);
}

TextForMimeData ArchivedText(const QByteArray &body) {
	const auto object = QJsonDocument::fromJson(body).object();
	auto text = TextWithEntities{ object.value(u"text"_q).toString() };
	for (const auto &value : object.value(u"entities"_q).toArray()) {
		const auto row = value.toArray();
		if (row.size() != 4) continue;
		text.entities.push_back({ EntityType(row[0].toInt()), row[1].toInt(), row[2].toInt(), row[3].toString() });
	}
	return TextForMimeData::Rich(std::move(text));
}

void ObserveArchiveMessage(not_null<Main::Session*> session, const MTPMessage &message,
		MsgId id, MessageFlags flags, bool arrival) {
	if (!IsServerMsgId(id) || (flags & (MessageFlag::AdminLogEntry | MessageFlag::FakeHistoryItem))
		|| message.type() != mtpc_message || id.bare != message.c_message().vid().v) return;
	RememberDesktopRichPreview(session, message);
	Accept(session, message.c_message(), Observation::Baseline, "baseline", arrival);
}

void ObserveArchiveEdit(not_null<Main::Session*> session, const MTPMessage &message, int pts, bool channel) {
	RememberDesktopRichPreview(session, message);
	if (message.type() != mtpc_message || ObserveArchiveExpiredReplacement(session, message, pts)) return;
	Accept(session, message.c_message(), Observation::Edit,
		(channel ? QByteArray("channel-edit/") : QByteArray("edit/")) + QByteArray::number(pts));
}

void ObserveArchiveSentMetadata(not_null<Main::Session*> session, const MTPDmessage &message) {
	Accept(session, message, Observation::Baseline, "baseline", true);
}

void ObserveArchiveShortSent(not_null<Main::Session*> session, not_null<HistoryItem*> item,
		const MTPDupdateShortSentMessage &acknowledgement) {
	if (!IsServerMsgId(item->id) || item->id.bare != acknowledgement.vid().v || item->isScheduled() || item->shortcutId()) return;
	auto event = Event(item->history()->peer->id, item->id, Observation::Baseline, "baseline", item->history()->peer);
	Attach(acknowledgement.vmedia(), event);
	event.body = Pack(item->originalText(), item->from()->id, event.attachments);
	(void)Queue(session, std::move(event));
}

void ObserveArchiveDelete(not_null<Main::Session*> session, PeerId peer, const QVector<MTPint> &ids, int pts) {
	const auto token = "delete/" + QByteArray::number(pts);
	for (const auto &id : ids) {
		if (!IsServerMsgId(MsgId(id.v))) continue;
		auto event = Event(peer, MsgId(id.v), Observation::Delete, token);
		if (peer) event.preimage = Preimage(session->data().message(peer, MsgId(id.v)));
		(void)Queue(session, std::move(event));
	}
}

void RecordArchiveExpiry(not_null<HistoryItem*> item, bool consumed) {
	CancelArchiveAttachment(item);
	const auto peer = item->history()->peer;
	auto event = Event(peer->id, item->id, consumed ? Observation::Consume : Observation::Expire, "end", peer);
	event.preimage = Preimage(item);
	(void)Queue(&item->history()->session(), std::move(event));
}

}

#include "purple/purple_archive_capture.h"
#include "purple/purple_archive_codec.h"
#include <QtCore/QJsonArray>
#include <algorithm>

namespace Purple {
namespace {

template <typename T> QByteArray Bytes(const T &value) {
	const auto length = tl::count_length(value);
	if (length > kArchiveCodecByteLimit) {
		return {};
	}
	auto buffer = mtpBuffer();
	value.write(buffer);
	return QByteArray(reinterpret_cast<const char*>(buffer.constData()),
		buffer.size() * sizeof(mtpPrime));
}

template <typename T> QByteArray Bytes(const tl::conditional<T> &value) {
	return value ? Bytes(*value) : QByteArray();
}

QJsonArray DocumentAttributes(const QVector<MTPDocumentAttribute> &attributes) {
	auto result = QJsonArray();
	for (const auto &attribute : attributes) {
		attribute.match([&](const auto &data) {
			if constexpr (requires { data.vstickerset(); }) {
				const auto &set = data.vstickerset();
				auto identity = QJsonObject{{u"type"_q, QString::number(set.type())}};
				set.match([&](const auto &value) {
					if constexpr (requires {
									  value.vid();
									  value.vaccess_hash();
								  }) {
						identity.insert(u"id"_q, QString::number(value.vid().v));
					} else {
						identity.insert(
							u"tl"_q, QString::fromLatin1(Bytes(set).toBase64()));
					}
				});
				auto normalized = QJsonObject{
					{u"type"_q, QString::number(attribute.type())},
					{u"alt"_q, qs(data.valt())}, {u"set"_q, identity},
					{u"flags"_q, QString::fromLatin1(Bytes(data.vflags()).toBase64())}};
				if constexpr (requires { data.vmask_coords(); }) {
					normalized.insert(u"mask"_q,
						QString::fromLatin1(Bytes(data.vmask_coords()).toBase64()));
				}
				result.push_back(normalized);
			} else {
				result.push_back(QJsonObject{
					{u"tl"_q, QString::fromLatin1(Bytes(attribute).toBase64())}});
			}
		});
	}
	return result;
}

void Document(ArchiveContent &content, const MTPDocument &document) {
	if (document.type() != mtpc_document) {
		return;
	}
	const auto &data = document.c_document();
	const auto identity = u"document:"_q + QString::number(data.vid().v);
	if (std::any_of(content.media.begin(), content.media.end(),
			[&](const auto &entry) { return entry.identity == identity; })) {
		return;
	}
	content.media.push_back({identity, u"document"_q,
		{{u"mime"_q, qs(data.vmime_type())}, {u"size"_q, QString::number(data.vsize().v)},
			{u"attributes"_q, DocumentAttributes(data.vattributes().v)}}});
}

void Photo(ArchiveContent &content, const MTPPhoto &photo) {
	if (photo.type() != mtpc_photo) {
		return;
	}
	const auto identity = u"photo:"_q + QString::number(photo.c_photo().vid().v);
	if (std::none_of(content.media.begin(), content.media.end(),
			[&](const auto &entry) { return entry.identity == identity; })) {
		content.media.push_back({identity, u"photo"_q});
	}
}

QJsonArray Blocks(const QVector<MTPPageBlock> &blocks, int depth = 0) {
	auto result = QJsonArray();
	if (depth > 16) {
		return QJsonArray{QJsonObject{{u"opaqueTl"_q,
			QString::fromLatin1(Bytes(MTP_vector<MTPPageBlock>(blocks)).toBase64())}}};
	}
	for (const auto &block : blocks) {
		block.match([&](const auto &data) {
			using T = std::decay_t<decltype(data)>;
			auto object = QJsonObject{{u"type"_q, QString::number(block.type())}};
			if constexpr (std::is_same_v<T, MTPDpageBlockChannel>) {
				data.vchannel().match([&](const auto &chat) {
					object.insert(u"id"_q, QString::number(chat.vid().v));
					if constexpr (requires { chat.vtitle(); }) {
						object.insert(u"title"_q, qs(chat.vtitle()));
					}
				});
			} else if constexpr (
				requires { data.vblocks(); } || requires { data.vitems(); }
				|| requires { data.vcover(); }) {
				if constexpr (requires { data.vflags(); }) {
					object.insert(u"flagsTl"_q,
						QString::fromLatin1(Bytes(data.vflags()).toBase64()));
				}
				if constexpr (requires { data.vtitle(); }) {
					object.insert(u"titleTl"_q,
						QString::fromLatin1(Bytes(data.vtitle()).toBase64()));
				}
				if constexpr (requires { data.vcaption(); }) {
					object.insert(u"captionTl"_q,
						QString::fromLatin1(Bytes(data.vcaption()).toBase64()));
				}
				if constexpr (requires { data.vurl(); }) {
					object.insert(u"url"_q, qs(data.vurl()));
				}
				if constexpr (requires { data.vauthor(); }) {
					object.insert(u"author"_q, qs(data.vauthor()));
				}
				if constexpr (requires { data.vdate(); }) {
					object.insert(u"date"_q, data.vdate().v);
				}
				if constexpr (requires { data.vwebpage_id(); }) {
					object.insert(u"webpage"_q, QString::number(data.vwebpage_id().v));
				}
				if constexpr (requires { data.vauthor_photo_id(); }) {
					object.insert(
						u"authorPhoto"_q, QString::number(data.vauthor_photo_id().v));
				}
				if constexpr (requires { data.vstart(); }) {
					object.insert(u"startTl"_q,
						QString::fromLatin1(Bytes(data.vstart()).toBase64()));
				}
				if constexpr (requires { data.vtype(); }) {
					object.insert(u"listTypeTl"_q,
						QString::fromLatin1(Bytes(data.vtype()).toBase64()));
				}
				if constexpr (requires { data.vcover(); }) {
					object.insert(u"children"_q, Blocks({data.vcover()}, depth + 1));
				}
				if constexpr (requires { data.vblocks(); }) {
					object.insert(u"children"_q, Blocks(data.vblocks().v, depth + 1));
				}
				if constexpr (requires { data.vitems(); }) {
					using Item =
						typename std::decay_t<decltype(data.vitems().v)>::value_type;
					if constexpr (std::is_same_v<Item, MTPPageBlock>) {
						object.insert(u"children"_q, Blocks(data.vitems().v, depth + 1));
					} else {
						auto items = QJsonArray();
						for (const auto &item : data.vitems().v) {
							item.match([&](const auto &entry) {
								if constexpr (requires { entry.vblocks(); }) {
									auto list = QJsonObject{
										{u"children"_q,
											Blocks(entry.vblocks().v, depth + 1)},
										{u"flagsTl"_q,
											QString::fromLatin1(
												Bytes(entry.vflags()).toBase64())}};
									if constexpr (requires { entry.vnum(); }) {
										list.insert(u"numTl"_q,
											QString::fromLatin1(
												Bytes(entry.vnum()).toBase64()));
									}
									if constexpr (requires { entry.vvalue(); }) {
										list.insert(u"valueTl"_q,
											QString::fromLatin1(
												Bytes(entry.vvalue()).toBase64()));
									}
									if constexpr (requires { entry.vtype(); }) {
										list.insert(u"typeTl"_q,
											QString::fromLatin1(
												Bytes(entry.vtype()).toBase64()));
									}
									items.push_back(list);
								} else {
									items.push_back(QJsonObject{{u"tl"_q,
										QString::fromLatin1(Bytes(item).toBase64())}});
								}
							});
						}
						object.insert(u"items"_q, items);
					}
				}
			} else {
				object.insert(u"tl"_q, QString::fromLatin1(Bytes(block).toBase64()));
			}
			result.push_back(object);
		});
	}
	return result;
}

void Media(ArchiveEvent &event, const MTPMessageMedia &media) {
	auto &content = *event.content;
	if (media.type() == mtpc_messageMediaEmpty) {
		return;
	}
	if (media.type() == mtpc_messageMediaDocument) {
		const auto &data = media.c_messageMediaDocument();
		if (const auto document = data.vdocument()) {
			Document(content, *document);
		}
		content.preserved.insert(u"spoiler"_q, data.is_spoiler());
	} else if (media.type() == mtpc_messageMediaPhoto) {
		const auto &data = media.c_messageMediaPhoto();
		if (const auto photo = data.vphoto()) {
			Photo(content, *photo);
		}
		if (const auto video = data.vvideo()) {
			Document(content, *video);
		}
		content.preserved.insert(u"spoiler"_q, data.is_spoiler());
	} else if (media.type() == mtpc_messageMediaWebPage) {
		const auto &page = media.c_messageMediaWebPage().vwebpage();
		if (page.type() == mtpc_webPage) {
			const auto &data = page.c_webPage();
			auto rich
				= QJsonObject{{u"format"_q, u"web-page"_q}, {u"url"_q, qs(data.vurl())}};
			for (const auto &[name, value] : std::vector<std::pair<QString, QString>>{
					 {u"title"_q, data.vtitle() ? qs(*data.vtitle()) : QString()},
					 {u"description"_q,
						 data.vdescription() ? qs(*data.vdescription()) : QString()},
					 {u"author"_q, data.vauthor() ? qs(*data.vauthor()) : QString()}}) {
				rich.insert(name, value);
			}
			if (const auto cached = data.vcached_page()) {
				event.completeness = ArchiveCompleteness::Opaque;
				const auto &body = cached->data();
				rich.insert(u"blocks"_q, Blocks(body.vblocks().v));
				for (const auto &photo : body.vphotos().v) {
					Photo(content, photo);
				}
				for (const auto &document : body.vdocuments().v) {
					Document(content, document);
				}
				if (body.is_part()) {
					event.completeness = ArchiveCompleteness::Partial;
					event.missing.content.push_back({{u"rich"_q, u"blocks"_q}});
				}
			}
			if (const auto photo = data.vphoto()) {
				Photo(content, *photo);
			}
			if (const auto document = data.vdocument()) {
				Document(content, *document);
			}
			content.rich = rich;
		} else {
			content.rich
				= QJsonObject{{u"format"_q, u"web-page"_q}, {u"pending"_q, true}};
			event.completeness = ArchiveCompleteness::Partial;
			event.missing.content.push_back({{u"rich"_q}});
		}
	} else if (media.type() == mtpc_messageMediaGame) {
		const auto &game = media.c_messageMediaGame().vgame().data();
		content.rich = QJsonObject{{u"format"_q, u"game"_q},
			{u"id"_q, QString::number(game.vid().v)}, {u"title"_q, qs(game.vtitle())},
			{u"description"_q, qs(game.vdescription())},
			{u"name"_q, qs(game.vshort_name())}};
		Photo(content, game.vphoto());
		if (const auto document = game.vdocument()) {
			Document(content, *document);
		}
	} else if (media.type() == mtpc_messageMediaStory) {
		const auto &story = media.c_messageMediaStory();
		content.rich = QJsonObject{{u"format"_q, u"story-reference"_q},
			{u"peerTl"_q, QString::fromLatin1(Bytes(story.vpeer()).toBase64())},
			{u"id"_q, story.vid().v}};
		event.completeness = ArchiveCompleteness::Partial;
		event.missing.content.push_back({{u"storyBody"_q}});
	} else if (media.type() == mtpc_messageMediaInvoice) {
		const auto &invoice = media.c_messageMediaInvoice();
		content.rich
			= QJsonObject{{u"format"_q, u"invoice"_q}, {u"title"_q, qs(invoice.vtitle())},
				{u"description"_q, qs(invoice.vdescription())},
				{u"currency"_q, qs(invoice.vcurrency())},
				{u"amount"_q, QString::number(invoice.vtotal_amount().v)},
				{u"start"_q, qs(invoice.vstart_param())}};
		event.completeness = ArchiveCompleteness::Partial;
		event.missing.content.push_back({{u"invoicePresentation"_q}});
	} else if (media.type() == mtpc_messageMediaToDo) {
		content.opaqueFormat = u"telegram-todo-tl"_q;
		content.opaque = Bytes(media.c_messageMediaToDo().vtodo());
		event.completeness = ArchiveCompleteness::Opaque;
	} else if (media.type() == mtpc_messageMediaPaidMedia) {
		const auto &paid = media.c_messageMediaPaidMedia();
		content.rich = QJsonObject{{u"format"_q, u"paid-media"_q},
			{u"stars"_q, QString::number(paid.vstars_amount().v)}};
		for (const auto &item : paid.vextended_media().v) {
			if (item.type() == mtpc_messageExtendedMedia) {
				Media(event, item.c_messageExtendedMedia().vmedia());
			} else {
				event.completeness = ArchiveCompleteness::Partial;
				if (std::none_of(event.missing.content.begin(),
						event.missing.content.end(), [](const auto &slot) {
							return slot.path
								   == std::vector<QString>{u"paidMediaPreview"_q};
						})) {
					event.missing.content.push_back({{u"paidMediaPreview"_q}});
				}
			}
		}
	} else if (media.type() == mtpc_messageMediaVideoStream) {
		auto call = QJsonObject{{u"format"_q, u"video-stream"_q}};
		media.c_messageMediaVideoStream().vcall().match([&](const auto &data) {
			if constexpr (requires { data.vid(); }) {
				call.insert(u"id"_q, QString::number(data.vid().v));
			}
			if constexpr (requires { data.vslug(); }) {
				call.insert(u"slug"_q, qs(data.vslug()));
			}
			if constexpr (requires { data.vmsg_id(); }) {
				call.insert(u"message"_q, data.vmsg_id().v);
			}
		});
		content.rich = call;
	} else if (media.type() == mtpc_messageMediaPoll) {
		const auto &data = media.c_messageMediaPoll();
		content.opaqueFormat = u"telegram-poll-tl"_q;
		content.opaque = Bytes(data.vpoll());
		event.completeness = ArchiveCompleteness::Opaque;
	} else {
		content.opaqueFormat = u"telegram-media-tl"_q;
		content.opaque = Bytes(media);
		event.completeness = ArchiveCompleteness::Opaque;
	}
}

QJsonArray WireEntities(const QVector<MTPMessageEntity> &entities) {
	auto result = QJsonArray();
	for (const auto &entity : entities) {
		result.push_back(
			QJsonObject{{u"tl"_q, QString::fromLatin1(Bytes(entity).toBase64())}});
	}
	return result;
}

}

ArchiveChatKey DesktopArchiveChat(PeerId peer) {
	if (const auto id = peerToUser(peer)) {
		return {ArchiveChatKind::User, int64(id.bare)};
	}
	if (const auto id = peerToChat(peer)) {
		return {ArchiveChatKind::Group, int64(id.bare)};
	}
	if (const auto id = peerToChannel(peer)) {
		return {ArchiveChatKind::Channel, int64(id.bare)};
	}
	return {};
}

std::optional<ArchiveEvent> DesktopArchiveSnapshotData(const AccountKey &account,
	const MTPDmessage &data, ArchiveOrigin origin, QString id, int64 observedAt) {
	if (data.vid().v <= 0 || data.vquick_reply_shortcut_id()) {
		return {};
	}
	auto event = ArchiveEvent();
	event.key = {account, DesktopArchiveChat(peerFromMTP(data.vpeer_id())), data.vid().v};
	event.id = std::move(id);
	event.origin = origin;
	event.content = ArchiveContent();
	event.content->text = qs(data.vmessage());
	event.completeness = ArchiveCompleteness::Complete;
	if (const auto entities = data.ventities()) {
		event.content->entities = WireEntities(entities->v);
	}
	event.observation.observedAt = observedAt;
	event.observation.serverTime = data.vdate().v;
	if (const auto from = data.vfrom_id()) {
		const auto sender = DesktopArchiveChat(peerFromMTP(*from));
		event.observation.context.insert(
			u"sender"_q, QJsonObject{{u"kind"_q, int(sender.kind)},
							 {u"id"_q, QString::number(sender.id)}});
	}
	if (const auto grouped = data.vgrouped_id()) {
		event.observation.context.insert(u"album"_q, QString::number(grouped->v));
	}
	if (const auto author = data.vpost_author()) {
		event.observation.context.insert(u"postAuthor"_q, qs(*author));
	}
	if (const auto reply = data.vreply_to()) {
		reply->match(
			[&](const MTPDmessageReplyHeader &header) {
				const auto parent
					= header.vreply_to_peer_id()
						  ? DesktopArchiveChat(peerFromMTP(*header.vreply_to_peer_id()))
						  : event.key.chat;
				const auto message = header.vreply_to_msg_id().value_or(0);
				const auto top = header.vreply_to_top_id().value_or(message);
				event.observation.context.insert(
					u"reply"_q, QJsonObject{{u"kind"_q, int(parent.kind)},
									{u"parent"_q, QString::number(parent.id)},
									{u"message"_q, QString::number(message)},
									{u"top"_q, QString::number(top)},
									{u"forum"_q, header.is_forum_topic()},
									{u"scheduled"_q, header.is_reply_to_scheduled()}});
				if (header.is_forum_topic() && parent == event.key.chat && top > 0) {
					event.observation.topicId = top;
				}
			},
			[&](const MTPDmessageReplyStoryHeader &header) {
				const auto parent = DesktopArchiveChat(peerFromMTP(header.vpeer()));
				event.observation.context.insert(u"replyStory"_q,
					QJsonObject{{u"kind"_q, int(parent.kind)},
						{u"parent"_q, QString::number(parent.id)},
						{u"story"_q, QString::number(header.vstory_id().v)}});
			});
	}
	if (const auto media = data.vmedia()) {
		Media(event, *media);
	}
	if (const auto rich = data.vrich_message()) {
		const auto &body = rich->data();
		event.content->rich = QJsonObject{{u"format"_q, u"telegram-page-blocks"_q},
			{u"rtl"_q, body.is_rtl()}, {u"blocks"_q, Blocks(body.vblocks().v)}};
		for (const auto &photo : body.vphotos().v) {
			Photo(*event.content, photo);
		}
		for (const auto &document : body.vdocuments().v) {
			Document(*event.content, document);
		}
		event.completeness
			= body.is_part() ? ArchiveCompleteness::Partial : ArchiveCompleteness::Opaque;
		if (body.is_part()) {
			event.missing.content.push_back({{u"rich"_q, u"blocks"_q}});
		}
	}
	if (!event.missing.content.empty()) {
		event.completeness = ArchiveCompleteness::Partial;
	}
	return IsArchiveMessageKey(event.key) ? std::optional(event) : std::nullopt;
}

std::optional<ArchiveEvent> DesktopArchiveSnapshot(const AccountKey &account,
	const MTPMessage &message, ArchiveOrigin origin, QString id, int64 observedAt) {
	if (message.type() != mtpc_message) {
		return {};
	}
	auto event = DesktopArchiveSnapshotData(
		account, message.c_message(), origin, std::move(id), observedAt);
	if (!event) {
		return {};
	}
	event->observation.payloadFormat
		= u"telegram-message-tl:"_q + QString::number(MTP::details::kCurrentLayer);
	event->observation.payload = Bytes(message);
	if (event->observation.payload.isEmpty()) {
		event->observation.payloadFormat.clear();
	}
	if (tl::count_length(message) > kArchiveCodecByteLimit) {
		event->observation.context.insert(u"desktopSnapshotLimited"_q, true);
		event->missing.content.push_back({{u"opaqueStructure"_q}});
		event->completeness = ArchiveCompleteness::Partial;
	}
	return event;
}

bool DesktopArchiveReceivedId(MsgId id, MessageFlags flags, const MTPMessage &message) {
	return IsServerMsgId(id)
		   && !(flags & (MessageFlag::AdminLogEntry | MessageFlag::FakeHistoryItem))
		   && message.type() == mtpc_message && id.bare == message.c_message().vid().v;
}

std::optional<ArchiveEvent> DesktopArchiveShortSentSnapshot(
	ArchiveEvent event, const MTPDupdateShortSentMessage &acknowledgement) {
	if (!event.content || event.key.messageId <= 0
		|| event.key.messageId != acknowledgement.vid().v) {
		return {};
	}
	event.origin = ArchiveOrigin::Baseline;
	event.completeness = ArchiveCompleteness::Partial;
	if (event.missing.content.empty()) {
		event.missing.content.push_back({{u"observedStructure"_q}});
	}
	event.observation.serverTime = acknowledgement.vdate().v;
	event.observation.context.insert(u"desktopShortSendAcknowledged"_q, true);
	event.observation.context.insert(u"desktopSendMetadata"_q, true);
	event.observation.context.insert(u"acceptedPts"_q, acknowledgement.vpts().v);
	event.observation.payloadFormat
		= u"telegram-short-send-tl:"_q + QString::number(MTP::details::kCurrentLayer);
	event.observation.payload = Bytes(MTPUpdates(MTP_updateShortSentMessage(
		acknowledgement.vflags(), acknowledgement.vid(), acknowledgement.vpts(),
		acknowledgement.vpts_count(), acknowledgement.vdate(),
		acknowledgement.vmedia() ? *acknowledgement.vmedia()
			: MTPMessageMedia(MTP_messageMediaEmpty()),
		acknowledgement.ventities() ? *acknowledgement.ventities()
			: MTPVector<MTPMessageEntity>(MTP_vector<MTPMessageEntity>()),
		acknowledgement.vttl_period() ? *acknowledgement.vttl_period() : MTP_int(0))));
	if (const auto media = acknowledgement.vmedia()) {
		event.content->media.clear();
		Media(event, *media);
	}
	event.completeness = ArchiveCompleteness::Partial;
	return event;
}

}

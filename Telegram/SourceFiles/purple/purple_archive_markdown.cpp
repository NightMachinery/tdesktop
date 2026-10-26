#include "purple/purple_archive_markdown.h"

#include "purple/purple_article_markdown.h"
#include "purple/purple_copy.h"
#include "ui/text/text_utilities.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <cstring>

namespace Purple {
namespace {

QString JsonFence(const QJsonObject &object) {
	const auto json = QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented));
	auto longest = 0;
	auto run = 0;
	for (const auto ch : json) {
		run = ch == '`' ? run + 1 : 0;
		longest = std::max(longest, run);
	}
	const auto fence = QString(std::max(3, longest + 1), '`');
	return u"[Retained archive structure, JSON with original TL fields encoded as base64]\n\n"_q
		+ fence + u"json\n"_q + json + fence + '\n';
}

std::optional<EntityInText> Entity(const QJsonObject &value, const QString &text) {
	auto type = EntityType::Invalid;
	auto data = QString();
	auto offset = -1;
	auto length = -1;
	if (value.contains(u"tl"_q)) {
		const auto base64 = value.value(u"tl"_q).toString().toLatin1();
		const auto bytes = QByteArray::fromBase64(base64);
		if (value.size() != 1 || bytes.size() < sizeof(mtpPrime) || bytes.size() % sizeof(mtpPrime)
			|| bytes.toBase64() != base64) { return {}; }
		auto buffer = mtpBuffer(bytes.size() / sizeof(mtpPrime));
		std::memcpy(buffer.data(), bytes.constData(), bytes.size());
		auto from = buffer.constData();
		const auto end = from + buffer.size();
		auto entity = MTPMessageEntity();
		if (!entity.read(from, end) || from != end) { return {}; }
		entity.match([&](const auto &native) {
			offset = native.voffset().v;
			length = native.vlength().v;
		});
		switch (entity.type()) {
		case mtpc_messageEntityBold: type = EntityType::Bold; break;
		case mtpc_messageEntityItalic: type = EntityType::Italic; break;
		case mtpc_messageEntityStrike: type = EntityType::StrikeOut; break;
		case mtpc_messageEntityUnderline: type = EntityType::Underline; break;
		case mtpc_messageEntityCode: type = EntityType::Code; break;
		case mtpc_messageEntityPre:
			type = EntityType::Pre;
			data = qs(entity.c_messageEntityPre().vlanguage());
			break;
		case mtpc_messageEntityTextUrl:
			type = EntityType::CustomUrl;
			data = qs(entity.c_messageEntityTextUrl().vurl());
			break;
		case mtpc_messageEntityMentionName:
			type = EntityType::CustomUrl;
			data = u"tg://user?id="_q + QString::number(entity.c_messageEntityMentionName().vuser_id().v);
			break;
		case mtpc_messageEntityUrl: type = EntityType::Url; break;
		case mtpc_messageEntityEmail: type = EntityType::Email; break;
		case mtpc_messageEntityPhone: type = EntityType::Phone; break;
		case mtpc_messageEntityMention: type = EntityType::Mention; break;
		case mtpc_messageEntityHashtag: type = EntityType::Hashtag; break;
		case mtpc_messageEntityCashtag: type = EntityType::Cashtag; break;
		case mtpc_messageEntityBotCommand: type = EntityType::BotCommand; break;
		default: return {};
		}
	} else {
		if (value.size() != 4 || !value.value(u"type"_q).isDouble()
			|| !value.value(u"offset"_q).isDouble() || !value.value(u"length"_q).isDouble()
			|| !value.value(u"data"_q).isString()) { return {}; }
		for (const auto &key : {u"type"_q, u"offset"_q, u"length"_q}) {
			const auto number = value.value(key);
			if (number.toDouble() != number.toInt(-1)) { return {}; }
		}
		type = EntityType(value.value(u"type"_q).toInt(-1));
		offset = value.value(u"offset"_q).toInt(-1);
		length = value.value(u"length"_q).toInt(-1);
		data = value.value(u"data"_q).toString();
		const auto supported = {EntityType::Bold, EntityType::Italic, EntityType::StrikeOut,
			EntityType::Underline, EntityType::Code, EntityType::Pre, EntityType::CustomUrl,
			EntityType::Url, EntityType::Email, EntityType::Phone, EntityType::Mention,
			EntityType::Hashtag, EntityType::Cashtag, EntityType::BotCommand};
		if (std::find(supported.begin(), supported.end(), type) == supported.end()) { return {}; }
	}
	if (offset < 0 || length <= 0 || offset > text.size() || length > text.size() - offset) { return {}; }
	const auto splitsPair = [&](int boundary) {
		return boundary > 0 && boundary < text.size()
			&& text[boundary - 1].isHighSurrogate() && text[boundary].isLowSurrogate();
	};
	if (splitsPair(offset) || splitsPair(offset + length)) { return {}; }
	return EntityInText(type, offset, length, data);
}

}

QString DesktopArchiveMarkdownDigest(const ArchiveVersionSnapshot &snapshot) {
	const auto bytes = EncodeArchiveVersionSnapshot(snapshot);
	return bytes ? QString::fromLatin1(QCryptographicHash::hash(*bytes,
		QCryptographicHash::Sha256).toHex()) : QString();
}

bool DropDesktopArticleMarkdown(ArchiveEvent &event) {
	const auto key = u"desktopStructuralMarkdown"_q;
	if (!event.observation.context.contains(key)) { return false; }
	event.observation.context.remove(key);
	return true;
}

ArchiveSqlCapturePlan PlanDesktopArchiveMarkdownCapture(const ArchiveSqlSlice &slice,
		ArchiveEvent event, const ArchiveSqlSecurity &security,
		const ArchiveStoreEpoch *currentEpoch) {
	auto plan = PlanArchiveSqlCapture(slice, event, security, currentEpoch);
	if ((!plan.transaction || !plan.program) && DropDesktopArticleMarkdown(event)) {
		plan = PlanArchiveSqlCapture(slice, event, security, currentEpoch);
	}
	return plan;
}

void AttachDesktopArticleMarkdown(ArchiveEvent &event, const Iv::RichPage &page) {
	event.observation.context.remove(u"desktopStructuralMarkdown"_q);
	if (!event.content) { return; }
	const auto rendered = RenderArticleMarkdown(page);
	if (!rendered.error.isEmpty() || rendered.text.toUtf8().size() > 1024 * 1024) { return; }
	event.observation.context.insert(u"desktopStructuralMarkdown"_q,
		QJsonObject{{u"format"_q, u"desktop-article-markdown-v1"_q},
			{u"contentHash"_q, ArchiveContentHash(*event.content)},
			{u"representation"_q, event.key.representation},
			{u"partial"_q, page.part}, {u"text"_q, rendered.text}});

}

std::optional<QString> RenderDesktopArchiveMarkdown(const ArchiveVersionSnapshot &snapshot) {
	const auto encoded = EncodeArchiveVersionSnapshot(snapshot);
	if (!encoded) { return {}; }
	const auto &version = snapshot.version;
	const auto &content = version.content;
	const auto partial = version.completeness != ArchiveCompleteness::Complete;
	const auto prefix = version.completeness == ArchiveCompleteness::Opaque
		? u"[Opaque archived content: reconstructed from retained structure]\n\n"_q
		: partial ? u"[Partial archived content: only observed content is retained]\n\n"_q : QString();
	const auto fallback = [&] {
		const auto body = QJsonDocument::fromJson(*encoded).object().value(u"snapshot"_q).toObject();
		auto retained = QJsonObject{{u"content"_q, body.value(u"content"_q)},
			{u"payloadFormat"_q, version.observation.payloadFormat},
			{u"payload"_q, QString::fromLatin1(version.observation.payload.toBase64())}};
		if (!snapshot.preserved.isEmpty()) { retained.insert(u"snapshotMetadata"_q, snapshot.preserved); }
		if (!version.preserved.isEmpty()) { retained.insert(u"versionMetadata"_q, version.preserved); }
		if (!version.observation.preserved.isEmpty()) {
			retained.insert(u"observationMetadata"_q, version.observation.preserved);
		}
		return prefix + JsonFence(retained);
	};
	if (!snapshot.preserved.isEmpty() || !version.preserved.isEmpty()
		|| !version.observation.preserved.isEmpty()) { return fallback(); }
	if (!content.rich.isNull()) {
		if (!content.text.isEmpty() || !content.caption.isEmpty() || !content.entities.isEmpty()
			|| !content.opaque.isEmpty() || !content.opaqueFormat.isEmpty()
			|| !content.preserved.isEmpty()) { return fallback(); }
		const auto cache = version.observation.context.value(u"desktopStructuralMarkdown"_q).toObject();
		if (cache.size() == 5 && cache.value(u"format"_q) == u"desktop-article-markdown-v1"_q
			&& cache.value(u"contentHash"_q) == ArchiveContentHash(content)
			&& cache.value(u"representation"_q) == snapshot.key.representation
			&& cache.value(u"partial"_q).isBool() && cache.value(u"text"_q).isString()) {
			const auto extra = !partial && cache.value(u"partial"_q).toBool()
				? u"[Partial archived content: only observed content is retained]\n\n"_q : QString();
			return prefix + extra + cache.value(u"text"_q).toString();
		}
		return fallback();
	}
	if (!content.opaque.isEmpty() || !content.opaqueFormat.isEmpty() || !content.media.empty()
		|| !content.preserved.isEmpty() || (!content.text.isEmpty() && !content.caption.isEmpty())) {
		return fallback();
	}
	auto text = TextWithEntities();
	text.text = content.text.isEmpty() ? content.caption : content.text;
	for (const auto &value : content.entities) {
		if (!value.isObject()) { return fallback(); }
		const auto entity = Entity(value.toObject(), text.text);
		if (!entity) { return fallback(); }
		text.entities.push_back(*entity);
	}
	return prefix + CopyTextAsMarkdown(TextForMimeData::Rich(std::move(text)));
}

std::optional<QString> ReadDesktopArchiveMarkdown(
		DesktopArchiveStore &store, const DesktopArchiveMarkdownRequest &request) {
	if (!CanRunArchiveWork(request.epoch, store.epoch()) || request.digest.isEmpty()) { return {}; }
	const auto slice = store.ReadSlice(request.key, {}, request.version);
	const auto version = slice ? FindArchiveVersion(slice->state, request.key, request.version) : nullptr;
	if (!version || ArchiveGenerationFor(slice->state, request.key.chat) != request.generation) { return {}; }
	auto snapshot = ArchiveVersionSnapshot{request.key, *version};
	for (const auto &metadata : slice->versionMetadata) {
		if (metadata.key == request.key && metadata.versionId == request.version) {
			snapshot.preserved = metadata.preserved;
		}
	}
	if (DesktopArchiveMarkdownDigest(snapshot) != request.digest) { return {}; }
	const auto choice = ChooseCopySource(slice->state, {request.key, version->id, version->revision,
		ArchiveContentHash(version->content), request.key.representation, ArchiveCopyExtent::FullBody,
		ArchiveCopyTarget::ArchivedVersion, request.generation});
	if (!choice.validTarget || !store.Current()) { return {}; }
	if (choice.kind == ArchiveCopySourceKind::GenuineSource) {
		return DecodeDesktopMarkdownSource(choice.bytes);
	}
	const auto rendered = RenderDesktopArchiveMarkdown(snapshot);
	return rendered && !rendered->isEmpty() ? rendered : std::nullopt;
}

}

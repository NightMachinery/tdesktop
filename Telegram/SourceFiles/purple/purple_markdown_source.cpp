#include "purple/purple_markdown_source.h"

#include "purple/purple_archive_codec.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QStringConverter>

namespace Purple {

std::optional<DesktopMarkdownSourceTarget> DesktopMarkdownTarget(
		DesktopArchiveStore &store,
		const ArchiveMessageKey &key,
		const QString &version) {
	const auto slice = store.ReadSlice(key, {}, version);
	const auto message = slice ? FindArchiveMessage(slice->state, key) : nullptr;
	const auto retained = !message || message->versions.empty() ? nullptr
		: version.isEmpty() ? &message->versions.back()
		: FindArchiveVersion(slice->state, key, version);
	if (!retained || message->payloadPurged || !store.Current()) {
		return {};
	}
	return DesktopMarkdownSourceTarget{{key, retained->id, retained->revision,
		ArchiveContentHash(retained->content), key.representation,
		ArchiveCopyExtent::FullBody, ArchiveCopyTarget::CurrentMessage,
		ArchiveGenerationFor(slice->state, key.chat)}, store.epoch()};
}

bool AttachDesktopMarkdownSource(
		DesktopArchiveStore &store,
		const DesktopMarkdownSourceTarget &target,
		ArchiveSource source,
		std::function<bool()> current) {
	const auto &request = target.request;
	if (!current || !current() || source.bytes.size() > kDesktopMarkdownSourceLimit
		|| !CanRunArchiveWork(target.epoch, store.epoch())) {
		return false;
	}
	const auto slice = store.ReadSlice(request.key, {}, request.version);
	const auto version = slice ? FindArchiveVersion(slice->state, request.key, request.version) : nullptr;
	if (!version || version->revision != request.revision
		|| ArchiveContentHash(version->content) != request.contentHash
		|| ArchiveGenerationFor(slice->state, request.key.chat) != request.generation
		|| version->completeness != ArchiveCompleteness::Complete) {
		return false;
	}
	source.contentHash = request.contentHash;
	source.revision = request.revision;
	source.representation = request.displayedRepresentation;
	if (!source.binding) {
		source.binding = ArchiveSourceBinding();
	}
	source.binding->key = request.key;
	source.binding->version = request.version;
	auto event = ArchiveEvent();
	event.key = request.key;
	event.id = QString::fromLatin1(QCryptographicHash::hash(
		request.version.toUtf8() + request.revision.toUtf8() + source.bytes
			+ QByteArray::number(int(source.kind)), QCryptographicHash::Sha256).toHex());
	event.origin = source.kind == ArchiveSourceKind::ComposeMarkdown
		? ArchiveOrigin::SendMetadata : ArchiveOrigin::Hydration;
	event.generation = request.generation;
	event.targetVersion = request.version;
	event.targetRevision = request.revision;
	event.expectedContentHash = request.contentHash;
	event.observation = version->observation;
	event.source = std::move(source);
	if (event.origin == ArchiveOrigin::Hydration) {
		event.content = version->content;
		event.missing = version->missing;
		event.completeness = version->completeness;
	}
	const auto plan = PlanArchiveSqlCapture(*slice, event, store.security(), &target.epoch);
	return current() && plan.transaction && plan.program
		&& store.Execute(plan.program, std::move(current));
}

bool IsDesktopMarkdownFile(const ArchiveMediaDescriptor &document) {
	const auto name = document.attributes.value(u"name"_q).toString();
	const auto mime = document.attributes.value(u"mime"_q).toString()
		.section(';', 0, 0).trimmed().toLower();
	return document.kind == u"document"_q
		&& (name.endsWith(u".md"_q, Qt::CaseInsensitive)
			|| name.endsWith(u".markdown"_q, Qt::CaseInsensitive)
			|| name.endsWith(u".mkd"_q, Qt::CaseInsensitive)
			|| name.endsWith(u".mdown"_q, Qt::CaseInsensitive)
			|| name.endsWith(u".mkdn"_q, Qt::CaseInsensitive)
			|| name.endsWith(u".mdwn"_q, Qt::CaseInsensitive)
			|| mime == u"text/markdown"_q
			|| mime == u"text/x-markdown"_q
			|| mime == u"text/x-web-markdown"_q);
}

bool CaptureDesktopMarkdownFile(
		DesktopArchiveStore &store,
		const ArchiveMessageKey &key,
		const QString &version,
		const QString &identity) {
	const auto target = DesktopMarkdownTarget(store, key, version);
	const auto slice = target ? store.ReadSlice(key, {}, version) : std::nullopt;
	const auto retained = slice ? FindArchiveVersion(slice->state, key, version) : nullptr;
	if (!retained || retained->source || !retained->content.text.isEmpty()
		|| !retained->content.caption.isEmpty() || !retained->content.entities.isEmpty()
		|| !retained->content.rich.isNull() || !retained->content.opaque.isEmpty()
		|| retained->content.media.size() != 1
		|| retained->content.media.front().identity != identity
		|| !IsDesktopMarkdownFile(retained->content.media.front())) {
		return false;
	}
	const auto size = retained->content.media.front().attributes.value(u"size"_q).toString();
	auto ok = false;
	const auto expected = size.toULongLong(&ok);
	if (!ok || expected > kDesktopMarkdownSourceLimit) {
		return false;
	}
	auto source = ArchiveSource();
	source.kind = ArchiveSourceKind::ReceivedMarkdownFile;
	if (!store.ReadMedia(key, version, identity, [&](const QByteArray &part) {
		if (part.size() > kDesktopMarkdownSourceLimit - source.bytes.size()) {
			return false;
		}
		source.bytes += part;
		return true;
	}) || uint64_t(source.bytes.size()) != expected) {
		return false;
	}
	return AttachDesktopMarkdownSource(store, *target, std::move(source), [&] { return store.Current(); });
}

ArchiveCopySourceChoice ChooseDesktopCurrentMarkdown(
		DesktopArchiveStore &store, const DesktopMarkdownBody &body) {
	const auto target = DesktopMarkdownTarget(store, body.key);
	const auto slice = target ? store.ReadSlice(body.key, {}, target->request.version) : std::nullopt;
	const auto version = slice ? FindArchiveVersion(slice->state, body.key, target->request.version) : nullptr;
	if (!version || version->observation.serverTime != body.serverTime
		|| version->observation.context.value(u"editDate"_q).toInt() != body.editDate) {
		return {};
	}
	const auto &content = version->content;
	if (body.document.isEmpty()) {
		if (content != body.content) { return {}; }
	} else {
		if (!body.content.text.isEmpty() || !body.content.entities.isEmpty()
			|| !content.text.isEmpty() || !content.caption.isEmpty() || !content.entities.isEmpty()
			|| !content.rich.isNull() || !content.opaque.isEmpty() || content.media.size() != 1) {
			return {};
		}
		const auto &document = content.media.front();
		if (document.identity != body.document || !IsDesktopMarkdownFile(document)
			|| document.attributes.value(u"name"_q).toString() != body.name
			|| document.attributes.value(u"mime"_q).toString() != body.mime
			|| document.attributes.value(u"size"_q).toString() != QString::number(body.size)) {
			return {};
		}
	}
	return ChooseCopySource(slice->state, target->request);
}

std::optional<QString> DecodeDesktopMarkdownSource(const QByteArray &bytes) {
	auto decoder = QStringDecoder(QStringDecoder::Utf8, QStringConverter::Flag::ConvertInitialBom);
	auto text = QString(decoder(bytes));
	return decoder.hasError() ? std::nullopt : std::optional(std::move(text));
}

}

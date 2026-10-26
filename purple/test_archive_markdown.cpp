#include "purple/purple_archive_markdown.h"
#include "purple/purple_archive_journal.h"
#include "purple/purple_markdown_publication.h"
#include "purple/purple_archive_files.h"
#include "purple/purple_archive_sql.h"
#include "purple/purple_article_markdown.h"
#include "tests/archive_test_support.h"
#include <QtCore/QDir>
#include <QtCore/QBuffer>
#include <QtCore/QCryptographicHash>
#include <future>
#include <QtCore/QJsonDocument>
#include <QtGui/QGuiApplication>

using namespace Purple;
using namespace ArchiveTest;

namespace {

DesktopArchiveOptions Options(const QString &root) {
	CHECK(QDir().mkpath(root));
	CHECK(DesktopArchiveDirectory(root + u"/profile"_q));
	auto options = DesktopArchiveOptions{root + u"/archives"_q,
		root + u"/profile"_q, {0, 123}, u"0"_q, QByteArray(256, 'k'), true};
	options.loginAuthority = QByteArray(32, 'a');
	options.loginAuthKeyId = QByteArray(16, '1');
	options.loginAuthKeyIds = options.loginAuthKeyId;
	return options;
}

ArchiveVersionSnapshot Snapshot(ArchiveEvent event) {
	const auto transaction = PlanArchiveEvent(Empty(), event);
	CHECK(transaction);
	return {event.key, transaction.next.messages.front().versions.front()};
}

DesktopArchiveMarkdownRequest Request(DesktopArchiveStore &store, const QString &id = {},
		const ArchiveMessageKey &key = Key()) {
	const auto slice = store.ReadSlice(key, {}, id);
	CHECK(slice.has_value());
	const auto message = FindArchiveMessage(slice->state, key);
	CHECK(message && !message->versions.empty());
	const auto version = id.isEmpty() ? &message->versions.back()
		: FindArchiveVersion(slice->state, key, id);
	CHECK(version);
	const auto snapshot = ArchiveSqlVersionSnapshot(key, *version, slice->versionMetadata);
	return {store.epoch(), key, version->id, DesktopArchiveMarkdownDigest(snapshot),
		ArchiveGenerationFor(slice->state, key.chat)};
}

void Rendering() {
	Section = "retained structural rendering";
	auto snapshot = Snapshot(Event(Empty(), u"ordinary"_q, 1, u"Literal * characters"_q));
	CHECK(RenderDesktopArchiveMarkdown(snapshot).value_or(QString()).contains(u"Literal"_q));
	snapshot.version.completeness = ArchiveCompleteness::Partial;
	CHECK(RenderDesktopArchiveMarkdown(snapshot).value_or(QString()).startsWith(u"[Partial archived content:"_q));
	snapshot.version.completeness = ArchiveCompleteness::Complete;
	const auto entity = MTPMessageEntity(MTP_messageEntityBold(MTP_int(0), MTP_int(7)));
	auto buffer = mtpBuffer();
	entity.write(buffer);
	const auto bytes = QByteArray(reinterpret_cast<const char*>(buffer.constData()), buffer.size() * sizeof(mtpPrime));
	snapshot.version.content.entities = {QJsonObject{{u"tl"_q, QString::fromLatin1(bytes.toBase64())}}};
	CHECK(!RenderDesktopArchiveMarkdown(snapshot).value_or(QString()).contains(u"[Retained archive structure"_q));
	snapshot.version.content.entities = {QJsonObject{{u"tl"_q, u"invalid-base64"_q}}};
	CHECK(RenderDesktopArchiveMarkdown(snapshot).value_or(QString()).contains(u"invalid-base64"_q));
	snapshot.version.content.entities = {QJsonObject{{u"type"_q, 1}, {u"offset"_q, 0.5}, {u"length"_q, 1}, {u"data"_q, u""_q}}};
	CHECK(!RenderDesktopArchiveMarkdown(snapshot));
	snapshot.version.content.entities = {QJsonObject{{u"type"_q, int(EntityType::Bold)}, {u"offset"_q, 0}, {u"length"_q, 999}, {u"data"_q, u""_q}}};
	CHECK(RenderDesktopArchiveMarkdown(snapshot).value_or(QString()).contains(u"[Retained archive structure"_q));
	snapshot.version.content.entities = {};
	snapshot.version.content.opaqueFormat = u"unknown-future-content"_q;
	snapshot.version.content.opaque = "```\n````\nretained bytes";
	snapshot.version.observation.payloadFormat = u"retained-tl"_q;
	snapshot.version.observation.payload = snapshot.version.content.opaque;
	const auto unknown = RenderDesktopArchiveMarkdown(snapshot).value_or(QString());
	CHECK(unknown.contains(QString::fromLatin1(snapshot.version.content.opaque.toBase64())));
	CHECK(unknown.contains(u"unknown-future-content"_q));
	snapshot.version.content.opaqueFormat.clear();
	snapshot.version.content.opaque.clear();
	snapshot.version.content.preserved.insert(u"futureField"_q, u"`````"_q);
	const auto fenced = RenderDesktopArchiveMarkdown(snapshot).value_or(QString());
	CHECK(fenced.contains(u"``````json\n"_q));
	CHECK(fenced.contains(u"futureField"_q));

	snapshot.version.content = ArchiveContent();
	snapshot.version.content.text = u"A😀B"_q;
	snapshot.version.content.entities = {QJsonObject{{u"type"_q, int(EntityType::Bold)},
		{u"offset"_q, 2}, {u"length"_q, 1}, {u"data"_q, u""_q}}};
	CHECK(RenderDesktopArchiveMarkdown(snapshot).value_or(QString()).contains(u"[Retained archive structure"_q));
	snapshot.version.content.entities[0] = QJsonObject{{u"type"_q, int(EntityType::Bold)},
		{u"offset"_q, 1}, {u"length"_q, 2}, {u"data"_q, u""_q}};
	CHECK(!RenderDesktopArchiveMarkdown(snapshot).value_or(QString()).contains(u"[Retained archive structure"_q));
	snapshot.version.preserved.insert(u"futureBlock"_q, u"retained version value"_q);
	CHECK(RenderDesktopArchiveMarkdown(snapshot).value_or(QString()).contains(u"retained version value"_q));
	snapshot.preserved.insert(u"futureEnvelope"_q, u"retained envelope value"_q);
	CHECK(RenderDesktopArchiveMarkdown(snapshot).value_or(QString()).contains(u"retained envelope value"_q));

	Section = "exact captured article writer result";
	auto page = Iv::RichPage();
	auto block = Iv::RichPage::Block();
	block.kind = Iv::RichPage::BlockKind::Paragraph;
	block.text.text.text = u"Observed article paragraph"_q;
	page.blocks.push_back(block);
	auto event = Event(Empty(), u"article"_q, 1, {});
	event.content->rich = QJsonObject{{u"format"_q, u"telegram-page-blocks"_q}, {u"blocks"_q, QJsonArray()}};
	AttachDesktopArticleMarkdown(event, page);
	const auto rendered = RenderArticleMarkdown(page);
	CHECK(rendered.error.isEmpty());
	auto article = Snapshot(event);
	CHECK(RenderDesktopArchiveMarkdown(article) == rendered.text);
	article.version.completeness = ArchiveCompleteness::Opaque;
	CHECK(RenderDesktopArchiveMarkdown(article).value_or(QString()).startsWith(u"[Opaque archived content:"_q));
	article.version.completeness = ArchiveCompleteness::Complete;
	article.version.content.rich = QJsonObject{{u"unknown"_q, u"retained"_q}};
	CHECK(RenderDesktopArchiveMarkdown(article).value_or(QString()).contains(u"unknown"_q));
	event.observation.context = {};
	page.part = true;
	event.completeness = ArchiveCompleteness::Partial;
	AttachDesktopArticleMarkdown(event, page);
	CHECK(!event.observation.context.contains(u"desktopStructuralMarkdown"_q));
	article = Snapshot(event);
	CHECK(RenderDesktopArchiveMarkdown(article).value_or(QString()).startsWith(u"[Partial archived content:"_q));
	article.version.observation.context = {};
	CHECK(RenderDesktopArchiveMarkdown(article).value_or(QString()).contains(u"telegram-page-blocks"_q));
}

void OptionalCacheCases(DesktopArchiveStore &store) {
	Section = "actual retained-row optional cache admission";
	for (const auto completeness : {ArchiveCompleteness::Complete, ArchiveCompleteness::Opaque}) {
		const auto text = QString(256 * 1024, 'x');
		auto page = Iv::RichPage();
		auto paragraph = Iv::RichPage::Block();
		paragraph.kind = Iv::RichPage::BlockKind::Paragraph;
		paragraph.text.text.text = text;
		page.blocks.push_back(paragraph);
		auto event = Event(Empty(), u"retained-cache"_q, 1, {});
		event.content->rich = QJsonObject{{u"format"_q, u"retained-native-article"_q}, {u"text"_q, text}};
		event.completeness = completeness;
		auto state = PlanArchiveEvent(Empty(), event).next;
		CHECK(IsArchiveState(state));
		const auto initial = EncodeArchiveRow({ArchiveVersionSnapshot{Key(), state.messages.front().versions.front()}});
		CHECK(initial.has_value());
		state.messages.front().versions.front().preserved.insert(u"futureRetainedMetadata"_q,
			QString(kArchiveCodecByteLimit - initial->size() - 64 * 1024, 'p'));
		CHECK(IsArchiveState(state));
		event.id = u"observed-cache-again"_q;
		event.order = 2;
		CHECK(PlanArchiveEvent(state, event));
		AttachDesktopArticleMarkdown(event, page);
		CHECK(event.observation.context.contains(u"desktopStructuralMarkdown"_q));
		const auto plan = PlanDesktopArchiveMarkdownCapture({state}, event, store.security());
		CHECK(plan.transaction && plan.program);
		const auto &retained = plan.transaction.next.messages.front().versions.front();
		CHECK(retained.preserved == state.messages.front().versions.front().preserved);
		CHECK(!retained.observation.context.contains(u"desktopStructuralMarkdown"_q));
		CHECK(EncodeArchiveRow({ArchiveVersionSnapshot{Key(), retained}}).has_value());
		auto fitting = state;
		fitting.messages.front().versions.front().preserved = {};
		const auto fit = PlanDesktopArchiveMarkdownCapture({fitting}, event, store.security());
		CHECK(fit.transaction && fit.program);
		CHECK(fit.transaction.next.messages.front().versions.front().observation.context.contains(u"desktopStructuralMarkdown"_q));
		auto envelope = fitting;
		const auto snapshot = ArchiveVersionSnapshot{Key(), envelope.messages.front().versions.front()};
		auto metadata = QJsonObject{{u"futureEnvelope"_q,
			QString(kArchiveCodecByteLimit - EncodeArchiveRow({snapshot})->size() - 64 * 1024, 'e')}};
		const auto slice = ArchiveSqlSlice{envelope, {{Key(), snapshot.version.id, metadata}}};
		const auto root = PlanDesktopArchiveMarkdownCapture(slice, event, store.security());
		CHECK(root.transaction && root.program);
		CHECK(!root.transaction.next.messages.front().versions.front().observation.context.contains(u"desktopStructuralMarkdown"_q));
		const auto full = ArchiveSqlVersionSnapshot(Key(), root.transaction.next.messages.front().versions.front(), slice.versionMetadata);
		CHECK(full.preserved == metadata);
		CHECK(EncodeArchiveRow({full}).has_value());
	}
	Section = "optional cache outbox admission";
	const auto access = store.JournalAccess();
	CHECK(access.has_value());
	auto journal = DesktopArchiveJournal(*access);
	CHECK(journal.Open());
	const auto key = Key({0, 123}, 45, 70);
	const auto text = QString(40 * 1024, 'j');
	auto event = Event(Empty(), u"outbox-optional-cache"_q, 1, {}, ArchiveOrigin::Baseline, key);
	event.content->rich = QJsonObject{{u"format"_q, u"retained-native-article"_q}, {u"text"_q, text}};
	auto page = Iv::RichPage();
	auto paragraph = Iv::RichPage::Block();
	paragraph.kind = Iv::RichPage::BlockKind::Paragraph;
	paragraph.text.text.text = text;
	page.blocks.push_back(paragraph);
	AttachDesktopArticleMarkdown(event, page);
	CHECK(event.observation.context.contains(u"desktopStructuralMarkdown"_q));
	CHECK(journal.Append(event, 64 * 1024));
	CHECK(journal.Drain(store));
	const auto retained = store.ReadSlice(key);
	CHECK(retained && !retained->state.messages.empty());
	if (retained && !retained->state.messages.empty()) {
		const auto &version = retained->state.messages.front().versions.front();
		CHECK(version.content == *event.content);
		CHECK(!version.observation.context.contains(u"desktopStructuralMarkdown"_q));
	}
}

void ExactVersions(const QString &root) {
	Section = "exact historical source, digest and independent publication";
	auto store = DesktopArchiveStore();
	CHECK(store.Open(Options(root)) == DesktopArchiveStatus::Ready);
	OptionalCacheCases(store);
	CHECK(store.Capture(Event(Empty(), u"baseline"_q, 1, u"first body"_q)).retained);
	const auto beforeSource = Request(store);
	const auto target = DesktopMarkdownTarget(store, Key());
	CHECK(target.has_value());
	auto source = ArchiveSource();
	source.bytes = "**genuine first source**\n";
	source.binding = ArchiveSourceBinding{{}, {}, -99, 12345};
	CHECK(AttachDesktopMarkdownSource(store, *target, source, [] { return true; }));
	CHECK(!ReadDesktopArchiveMarkdown(store, beforeSource));
	const auto first = Request(store);
	CHECK(ReadDesktopArchiveMarkdown(store, first) == u"**genuine first source**\n"_q);
	const auto emptyKey = Key({0, 123}, 45, 68);
	CHECK(store.Capture(Event(Empty(), u"empty-source"_q, 2, {}, ArchiveOrigin::Baseline, emptyKey)).retained);
	const auto emptyTarget = DesktopMarkdownTarget(store, emptyKey);
	CHECK(emptyTarget.has_value());
	auto emptySource = ArchiveSource();
	emptySource.binding = ArchiveSourceBinding{{}, {}, -100, 12346};
	CHECK(AttachDesktopMarkdownSource(store, *emptyTarget, emptySource, [] { return true; }));
	const auto empty = ReadDesktopArchiveMarkdown(store, Request(store, {}, emptyKey));
	CHECK(empty.has_value() && empty->isEmpty());
	const auto fileKey = Key({0, 123}, 45, 69);
	const auto fileBytes = QByteArray("# exact file source\n");
	auto file = Event(Empty(), u"file-source"_q, 3, {}, ArchiveOrigin::Baseline, fileKey);
	file.content->media.push_back({u"document:321"_q, u"document"_q,
		{{u"name"_q, u"source.MKDN"_q}, {u"mime"_q, u"text/plain"_q},
			{u"size"_q, QString::number(fileBytes.size())}}});
	CHECK(store.Capture(file).retained);
	const auto fileTarget = DesktopMarkdownTarget(store, fileKey);
	CHECK(fileTarget.has_value());
	auto input = QBuffer();
	input.setData(fileBytes);
	CHECK(input.open(QIODevice::ReadOnly));
	const auto hash = QString::fromLatin1(QCryptographicHash::hash(fileBytes, QCryptographicHash::Sha256).toHex());
	CHECK(store.CopyMedia(fileKey, fileTarget->request.version, u"document:321"_q, input,
		{hash, uint64_t(fileBytes.size())}));
	CHECK(CaptureDesktopMarkdownFile(store, fileKey, fileTarget->request.version, u"document:321"_q));
	const auto fileRequest = Request(store, {}, fileKey);
	CHECK(ReadDesktopArchiveMarkdown(store, fileRequest) == QString::fromUtf8(fileBytes));
	const auto old = store.ReadSlice(Key());
	CHECK(store.Capture(Event(old->state, u"edit"_q, 2, u"newer body"_q, ArchiveOrigin::AuthoritativeEdit)).retained);
	CHECK(ReadDesktopArchiveMarkdown(store, first) == u"**genuine first source**\n"_q);
	const auto head = Request(store);
	CHECK(head.version != first.version);
	CHECK(ReadDesktopArchiveMarkdown(store, head).value_or(QString()).contains(u"newer body"_q));
	CHECK(!ReadDesktopArchiveMarkdown(store, head).value_or(QString()).contains(u"genuine first"_q));
	for (auto i = 0; i != 9; ++i) {
		auto wrong = first;
		switch (i) {
		case 0: wrong.digest[0] = wrong.digest[0] == '0' ? '1' : '0'; break;
		case 1: wrong.version = head.version; break;
		case 2: ++wrong.key.messageId; break;
		case 3: ++wrong.generation.account; break;
		case 4: wrong.epoch.account = {0, 456}; break;
		case 5: wrong.digest.clear(); break;
		case 6: wrong.epoch.storeId = u"different-store"_q; break;
		case 7: wrong.key.representation = u"translated"_q; break;
		case 8: wrong.key.account = {0, 456}; break;
		}
		CHECK(!ReadDesktopArchiveMarkdown(store, wrong));
	}
	const auto access = store.JournalAccess();
	CHECK(access && access->markdownPublication);
	const auto stamp = access->markdownPublication->stamp();
	auto delayed = ReadDesktopArchiveMarkdown(store, head);
	CHECK(delayed.has_value());
	CHECK(store.ReadSlice(Key())->state.leases.empty());
	auto clipboard = u"sentinel"_q;
	CHECK(!access->markdownPublication->publish(stamp, [] { return false; }, [&] { clipboard = *delayed; }));
	CHECK(clipboard == u"sentinel"_q);
	const auto before = store.ReadSlice(Key());
	CHECK(store.Capture(Event(before->state, u"delete"_q, 3, {}, ArchiveOrigin::AuthoritativeDelete)).retained);
	CHECK(!access->markdownPublication->publish(stamp, [] { return true; }, [&] { clipboard = *delayed; }));
	CHECK(clipboard == u"sentinel"_q);
	CHECK(ReadDesktopArchiveMarkdown(store, head).value_or(QString()).contains(u"newer body"_q));
	const auto deleted = store.OpenDeletedView({{{0, 123}, Key().chat}});
	CHECK(deleted.status == ArchiveQueryStatus::Ready);
	CHECK(deleted.entries.size() == 1);
	if (deleted.entries.size() == 1) {
		CHECK(deleted.entries.front().markdownDigest == head.digest);
	}
	CHECK(store.CloseDeletedView(deleted.view, deleted.epoch));
	const auto purge = store.PreparePurge(ArchivePurgeKind::Trashbin, {{0, 123}});
	CHECK(purge.confirmation.has_value());
	CHECK(store.BeginPurge(*purge.confirmation));
	CHECK(!ReadDesktopArchiveMarkdown(store, head));
	for (auto step = 0; step != 64 && store.MaintenancePending(); ++step) {
		CHECK(store.ContinuePurge().valid);
	}
	CHECK(!store.MaintenancePending());
	CHECK(!ReadDesktopArchiveMarkdown(store, head));
	CHECK(store.Close());
	CHECK(!access->markdownPublication->publish(access->markdownPublication->stamp(),
		[] { return true; }, [&] { clipboard = *delayed; }));
	CHECK(clipboard == u"sentinel"_q);
}

}

int main(int argc, char *argv[]) {
	QGuiApplication application(argc, argv);
	if (argc != 2) { return 2; }
	Rendering();
	std::async(std::launch::async, [root = QString::fromLocal8Bit(argv[1])] {
		ExactVersions(root);
	}).get();
	return Finish();
}

#include "purple/purple_markdown_source.h"
#include "purple/purple_compose_receipt.h"
#include "purple/purple_archive_capture.h"
#include "purple/purple_markdown_publication.h"
#include "purple/purple_markdown_current.h"
#include "purple/purple_markdown_delivery.h"
#include <QtCore/QMimeData>
#include "purple/purple_archive_journal.h"
#include <future>
#include <thread>
#include "purple/purple_archive_files.h"
#include "tests/archive_test_support.h"
#include <QtCore/QBuffer>
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>

using namespace Purple;
using namespace ArchiveTest;

namespace {

struct NativeMarkdownMessage {
	MTPflags<MTPDmessage::Flags> flags = {};
	MTPint id = {};
	MTPPeer from_id = {};
	MTPint from_boosts_applied = {};
	MTPstring from_rank = {};
	MTPPeer peer_id = {};
	MTPPeer saved_peer_id = {};
	MTPMessageFwdHeader fwd_from = {};
	MTPlong via_bot_id = {};
	MTPlong via_business_bot_id = {};
	MTPPeer guestchat_via_from = {};
	MTPMessageReplyHeader reply_to = {};
	MTPint date = {};
	MTPstring message = {};
	MTPMessageMedia media = {};
	MTPReplyMarkup reply_markup = {};
	MTPVector<MTPMessageEntity> entities = {};
	MTPint views = {};
	MTPint forwards = {};
	MTPMessageReplies replies = {};
	MTPint edit_date = {};
	MTPstring post_author = {};
	MTPlong grouped_id = {};
	MTPMessageReactions reactions = {};
	MTPVector<MTPRestrictionReason> restriction_reason = {};
	MTPint ttl_period = {};
	MTPint quick_reply_shortcut_id = {};
	MTPlong effect = {};
	MTPFactCheck factcheck = {};
	MTPint report_delivery_until_date = {};
	MTPlong paid_message_stars = {};
	MTPSuggestedPost suggested_post = {};
	MTPint schedule_repeat_period = {};
	MTPstring summary_from_language = {};
	MTPRichMessage rich_message = {};
	MTPmessage Build() const {
		return MTP_message(flags, id, from_id, from_boosts_applied, from_rank, peer_id, saved_peer_id, fwd_from, via_bot_id, via_business_bot_id, guestchat_via_from, reply_to, date, message, media, reply_markup, entities, views, forwards, replies, edit_date, post_author, grouped_id, reactions, restriction_reason, ttl_period, quick_reply_shortcut_id, effect, factcheck, report_delivery_until_date, paid_message_stars, suggested_post, schedule_repeat_period, summary_from_language, rich_message);
	}
};

DesktopArchiveOptions Options(const QString &root) {
	CHECK(QDir().mkpath(root));
	CHECK(DesktopArchiveDirectory(root + u"/profile"_q));
	auto options = DesktopArchiveOptions{
		root + u"/archives"_q, root + u"/profile"_q, {0, 123}, u"0"_q, QByteArray(256, 'k'), true};
	options.loginAuthority = QByteArray(32, 'a');
	options.loginAuthKeyId = QByteArray(16, '1');
	options.loginAuthKeyIds = options.loginAuthKeyId;
	return options;
}

ArchiveSource Compose(QByteArray bytes = "**original source**") {
	auto source = ArchiveSource();
	source.bytes = std::move(bytes);
	source.binding = ArchiveSourceBinding{{}, {}, -99, 12345};
	return source;
}

ArchiveCopySourceChoice Choice(DesktopArchiveStore &store,
		const DesktopMarkdownSourceTarget &target) {
	const auto slice = store.ReadSlice(target.request.key, {}, target.request.version);
	CHECK(slice.has_value());
	return slice ? ChooseCopySource(slice->state, target.request) : ArchiveCopySourceChoice();
}

void AcknowledgementCases() {
	Section = "real native acknowledgement validation";
	const auto key = Key();
	auto message = NativeMarkdownMessage();
	message.id = MTP_int(int(key.messageId));
	message.peer_id = MTP_peerChannel(MTP_long(key.chat.id));
	message.flags = MTP_flags(MTPDmessage::Flag::f_entities | MTPDmessage::Flag::f_out);
	message.date = MTP_int(90);
	message.message = MTP_string("original source");
	message.entities = MTP_vector<MTPMessageEntity>({MTP_messageEntityBold(MTP_int(0), MTP_int(15))});
	const auto expected = DesktopArchiveSnapshotData(key.account, message.Build().c_message(),
		ArchiveOrigin::Baseline, u"expected"_q, 100);
	CHECK(expected && expected->content);
	if (!expected || !expected->content) { return; }
	const auto acknowledge = MTP_updateMessageID(MTP_int(int(key.messageId)), MTP_long(555));
	const auto received = [&](const NativeMarkdownMessage &value) {
		return MTP_updateNewChannelMessage(value.Build(), MTP_int(1), MTP_int(1));
	};
	const auto updates = [&](QVector<MTPUpdate> values) {
		return MTP_updates(MTP_vector<MTPUpdate>(values), MTP_vector<MTPUser>(),
			MTP_vector<MTPChat>(), MTP_int(90), MTP_int(1));
	};
	const auto valid = updates({acknowledge, received(message)});
	CHECK(DesktopComposeAcknowledgement(key, 555, *expected->content, valid).has_value());
	CHECK(!DesktopComposeAcknowledgement(key, 556, *expected->content, valid));
	CHECK(!DesktopComposeAcknowledgement(key, 555, *expected->content, updates({received(message)})));
	CHECK(!DesktopComposeAcknowledgement(key, 555, *expected->content, updates({acknowledge, acknowledge, received(message)})));
	CHECK(!DesktopComposeAcknowledgement(key, 555, *expected->content, updates({acknowledge, received(message), received(message)})));
	for (auto index = 0; index != 7; ++index) {
		auto changed = message;
		switch (index) {
		case 0: changed.id = MTP_int(999); break;
		case 1: changed.peer_id = MTP_peerChannel(MTP_long(999)); break;
		case 2: changed.message = MTP_string("different body"); break;
		case 3: changed.entities = MTP_vector<MTPMessageEntity>(); break;
		case 4: changed.flags = MTP_flags(MTPDmessage::Flag::f_edit_date); changed.edit_date = MTP_int(92); break;
		case 5: changed.flags = MTP_flags(MTPDmessage::Flag::f_grouped_id); changed.grouped_id = MTP_long(123); break;
		case 6: changed.flags = MTP_flags(MTPDmessage::Flag::f_rich_message); break;
		}
		CHECK(!DesktopComposeAcknowledgement(key, 555, *expected->content, updates({acknowledge, received(changed)})));
	}
	const auto shortAck = MTP_updateShortSentMessage(MTP_flags(MTPDupdateShortSentMessage::Flags()),
		MTP_int(int(key.messageId)), MTP_int(1), MTP_int(1), MTP_int(90), MTP_messageMediaEmpty(),
		MTP_vector<MTPMessageEntity>(), MTP_int(0));
	CHECK(!DesktopComposeAcknowledgement(key, 555, *expected->content, shortAck));
}

void ComposeCases(const QString &root) {
	Section = "real encrypted source attachment and choice";
	auto store = DesktopArchiveStore();
	CHECK(store.Open(Options(root)) == DesktopArchiveStatus::Ready);
	CHECK(store.Capture(Event(Empty(), u"original"_q, 1, u"original source"_q)).retained);
	const auto target = DesktopMarkdownTarget(store, Key());
	CHECK(target.has_value());
	if (!target) { return; }
	CHECK(Choice(store, *target).kind == ArchiveCopySourceKind::StructuralWriter);
	const auto before = store.ReadSlice(Key());
	CHECK(AttachDesktopMarkdownSource(store, *target, Compose(), [] { return true; }));
	const auto after = store.ReadSlice(Key());
	CHECK(before && after);
	if (!before || !after) { return; }
	const auto &old = before->state.messages.front();
	const auto &now = after->state.messages.front();
	CHECK(now.order == old.order && now.versions.size() == old.versions.size());
	CHECK(now.versions.front().revision == old.versions.front().revision);
	CHECK(now.versions.front().content == old.versions.front().content);
	CHECK(now.versions.front().source->binding->localMessageId == -99);
	CHECK(now.versions.front().source->binding->randomId == 12345);
	auto displayed = DesktopMarkdownBody{Key(), old.versions.front().content};
	displayed.serverTime = 90;
	CHECK(ChooseDesktopCurrentMarkdown(store, displayed).kind == ArchiveCopySourceKind::GenuineSource);
	for (auto index = 0; index != 4; ++index) {
		auto stale = displayed;
		switch (index) {
		case 0: ++stale.serverTime; break;
		case 1: ++stale.editDate; break;
		case 2: stale.content.text += 'x'; break;
		case 3: stale.content.entities.push_back(QJsonObject{{u"unknown"_q, true}}); break;
		}
		CHECK(ChooseDesktopCurrentMarkdown(store, stale).kind == ArchiveCopySourceKind::StructuralWriter);
	}
	const auto access = store.JournalAccess();
	CHECK(access && access->markdownPublication);
	if (access) {
		auto journal = DesktopArchiveJournal(*access);
		CHECK(journal.access().markdownPublication == access->markdownPublication);
		const auto stamp = access->markdownPublication->stamp();
		CHECK(store.Gap(u"publication-revocation"_q, u"synthetic"_q, 100));
		auto delivered = false;
		CHECK(!access->markdownPublication->publish(stamp, [] { return true; }, [&] { delivered = true; }));
		CHECK(!delivered);
	}
	const auto genuine = Choice(store, *target);
	CHECK(genuine.validTarget && !genuine.partial);
	CHECK(genuine.kind == ArchiveCopySourceKind::GenuineSource && genuine.bytes == "**original source**");
	for (const auto extent : {ArchiveCopyExtent::Selection, ArchiveCopyExtent::Fragment}) {
		auto fragment = *target;
		fragment.request.extent = extent;
		CHECK(Choice(store, fragment).kind == ArchiveCopySourceKind::StructuralWriter);
	}
	for (auto index = 0; index != 7; ++index) {
		auto stale = *target;
		switch (index) {
		case 0: stale.request.key.account.userId += 1; break;
		case 1: stale.request.key.messageId += 1; break;
		case 2: stale.request.version = u"wrong-version"_q; break;
		case 3: stale.request.revision = u"wrong-revision"_q; break;
		case 4: stale.request.contentHash = QString(64, '0'); break;
		case 5: stale.request.displayedRepresentation = u"translation:fa"_q; break;
		case 6: ++stale.request.generation.chat; break;
		}
		const auto slice = store.ReadSlice(Key(), {}, target->request.version);
		CHECK(slice && !ChooseCopySource(slice->state, stale.request).validTarget);
		CHECK(!AttachDesktopMarkdownSource(store, stale, Compose(), [] { return true; }));
	}
	CHECK(store.Close());
	CHECK(store.Open(Options(root)) == DesktopArchiveStatus::Ready);
	CHECK(Choice(store, *target).bytes == "**original source**");
	auto edit = Event(store.ReadSlice(Key())->state, u"edited"_q, 2, u"new body"_q,
		ArchiveOrigin::AuthoritativeEdit);
	CHECK(store.Capture(edit).retained);
	CHECK(!Choice(store, *target).validTarget);
	auto historical = *target;
	historical.request.target = ArchiveCopyTarget::ArchivedVersion;
	CHECK(Choice(store, historical).bytes == "**original source**");
	CHECK(!AttachDesktopMarkdownSource(store, *target, Compose(), [] { return true; }));
	const auto directory = store.directory();
	for (const auto &name : {u"archive.sqlite"_q, u"archive.sqlite-wal"_q}) {
		auto file = QFile(directory + '/' + name);
		if (file.open(QIODevice::ReadOnly)) { CHECK(!file.readAll().contains("**original source**")); }
	}
	CHECK(store.Close());
}

void RevocationCases(const QString &root) {
	Section = "partial and commit authority";
	auto store = DesktopArchiveStore();
	CHECK(store.Open(Options(root)) == DesktopArchiveStatus::Ready);
	for (auto index = 0; index != 3; ++index) {
		auto event = Event(Empty(), u"candidate-"_q + QString::number(index), index + 1,
			u"body"_q, ArchiveOrigin::Baseline, Key({0, 123}, 45, 100 + index));
		if (!index) {
			event.completeness = ArchiveCompleteness::Partial;
			event.missing.content.push_back({{u"observedStructure"_q}});
		}
		CHECK(store.Capture(event).retained);
		const auto target = DesktopMarkdownTarget(store, event.key);
		CHECK(target.has_value());
		if (!target) { continue; }
		auto calls = 0;
		CHECK(!AttachDesktopMarkdownSource(store, *target, Compose(), [&] {
			return index == 2 ? ++calls < 4 : index == 0;
		}));
		if (index == 2) { CHECK(calls >= 4); }
		const auto slice = store.ReadSlice(event.key);
		CHECK(slice && !slice->state.messages.front().versions.front().source);
	}
	CHECK(store.Close());
}

void FileCases(const QString &root) {
	Section = "verified file bytes and historical hydration";
	auto store = DesktopArchiveStore();
	CHECK(store.Open(Options(root)) == DesktopArchiveStatus::Ready);
	const auto bytes = QByteArray("# Source\n\n**exact bytes**\n");
	auto event = Event(Empty(), u"file"_q, 1, QString());
	event.content->media.push_back({u"document:123"_q, u"document"_q,
		{{u"name"_q, u"source.MD"_q}, {u"mime"_q, u"application/octet-stream"_q},
			{u"size"_q, QString::number(bytes.size())}}});
	CHECK(IsDesktopMarkdownFile(event.content->media.front()));
	CHECK(store.Capture(event).retained);
	const auto target = DesktopMarkdownTarget(store, Key());
	CHECK(target.has_value());
	if (!target) { return; }
	CHECK(!CaptureDesktopMarkdownFile(store, Key(), target->request.version, u"document:123"_q));
	auto buffer = QBuffer();
	buffer.setData(bytes);
	CHECK(buffer.open(QIODevice::ReadOnly));
	const auto hash = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
	CHECK(store.CopyMedia(Key(), target->request.version, u"document:123"_q, buffer, {hash, uint64_t(bytes.size())}));
	CHECK(!CaptureDesktopMarkdownFile(store, Key(), target->request.version, u"document:wrong"_q));
	const auto baseline = store.ReadSlice(Key());
	auto edit = Event(baseline->state, u"file-edit"_q, 2, u"newer head"_q, ArchiveOrigin::AuthoritativeEdit);
	CHECK(store.Capture(edit).retained);
	CHECK(CaptureDesktopMarkdownFile(store, Key(), target->request.version, u"document:123"_q));
	auto historical = *target;
	historical.request.target = ArchiveCopyTarget::ArchivedVersion;
	CHECK(Choice(store, historical).bytes == bytes);
	CHECK(!Choice(store, *target).validTarget);
	const auto after = store.ReadSlice(Key(), {}, target->request.version);
	CHECK(after && after->state.messages.front().versions.size() == 2);
	CHECK(after && after->state.messages.front().versions.front().revision == target->request.revision);
	CHECK(after && after->state.messages.front().versions.back().content.text == u"newer head"_q);
	CHECK(store.Close());
}

void PurgeCases(const QString &root) {
	Section = "source shares purge ownership";
	auto store = DesktopArchiveStore();
	CHECK(store.Open(Options(root)) == DesktopArchiveStatus::Ready);
	CHECK(store.Capture(Event(Empty(), u"purge-original"_q, 1, u"body"_q)).retained);
	const auto target = DesktopMarkdownTarget(store, Key());
	CHECK(target.has_value());
	if (!target) { return; }
	CHECK(AttachDesktopMarkdownSource(store, *target, Compose(), [] { return true; }));
	const auto slice = store.ReadSlice(Key());
	CHECK(slice.has_value());
	CHECK(store.Capture(Event(slice->state, u"delete"_q, 2, {}, ArchiveOrigin::AuthoritativeDelete)).retained);
	const auto preview = store.PreparePurge(ArchivePurgeKind::Trashbin, {{0, 123}});
	CHECK(preview.confirmation.has_value());
	if (!preview.confirmation) { return; }
	CHECK(store.BeginPurge(*preview.confirmation));
	CHECK(!AttachDesktopMarkdownSource(store, *target, Compose(), [] { return true; }));
	for (auto step = 0; step != 64 && store.MaintenancePending(); ++step) {
		const auto result = store.ContinuePurge();
		CHECK(result.valid);
	}
	CHECK(!store.MaintenancePending());
	const auto purged = store.ReadSlice(Key());
	CHECK(purged && purged->state.messages.front().payloadPurged);
	CHECK(purged && purged->state.messages.front().versions.empty());
	CHECK(store.Close());
}

void RichHydrationCases(const QString &root) {
	Section = "exact observed rich body hydration";
	auto store = DesktopArchiveStore();
	CHECK(store.Open(Options(root)) == DesktopArchiveStatus::Ready);
	const auto block = MTP_pageBlockParagraph(MTP_textPlain(MTP_string("observed prefix")));
	auto message = NativeMarkdownMessage();
	message.id = MTP_int(int(Key().messageId));
	message.peer_id = MTP_peerChannel(MTP_long(Key().chat.id));
	message.flags = MTP_flags(MTPDmessage::Flag::f_rich_message);
	message.message = MTP_string("");
	message.date = MTP_int(90);
	message.rich_message = MTP_richMessage(MTP_flags(MTPDrichMessage::Flag::f_part),
		MTP_vector<MTPPageBlock>({block}), MTP_vector<MTPPhoto>(), MTP_vector<MTPDocument>());
	const auto partial = DesktopArchiveSnapshot({0, 123}, message.Build(), ArchiveOrigin::Baseline,
		u"rich-partial"_q, 100);
	CHECK(partial && partial->completeness == ArchiveCompleteness::Partial);
	if (!partial) { return; }
	CHECK(store.Capture(*partial).retained);
	const auto before = store.ReadSlice(Key());
	const auto &version = before->state.messages.front().versions.front();
	message.rich_message = MTP_richMessage(MTP_flags(MTPDrichMessage::Flags()),
		MTP_vector<MTPPageBlock>({block, block}), MTP_vector<MTPPhoto>(), MTP_vector<MTPDocument>());
	auto complete = DesktopArchiveSnapshot({0, 123}, message.Build(), ArchiveOrigin::Hydration,
		u"rich-complete"_q, 101);
	CHECK(complete.has_value());
	if (!complete) { return; }
	complete->targetVersion = version.id;
	complete->targetRevision = version.revision;
	complete->expectedContentHash = ArchiveContentHash(version.content);
	complete->generation = ArchiveGenerationFor(before->state, Key().chat);
	CHECK(store.Capture(*complete).retained);
	const auto after = store.ReadSlice(Key());
	CHECK(after && after->state.messages.front().versions.size() == 1);
	CHECK(after && after->state.messages.front().versions.front().revision == version.revision);
	CHECK(after && after->state.messages.front().versions.front().content == *complete->content);
	CHECK(after && after->state.messages.front().versions.front().observation.payload == complete->observation.payload);
	CHECK(store.Close());
}

void PublicationCases() {
	Section = "publication serialization and stale completion";
	auto publication = DesktopMarkdownPublication();
	const auto stamp = publication.stamp();
	auto delivered = 0;
	CHECK(publication.publish(stamp, [] { return true; }, [&] { ++delivered; }));
	CHECK(delivered == 1);
	CHECK(!publication.publish(stamp, [] { return false; }, [&] { ++delivered; }));
	auto entered = std::promise<void>();
	auto release = std::promise<void>();
	auto signal = release.get_future();
	auto writer = std::thread([&] {
		const auto lock = publication.write();
		entered.set_value();
		signal.wait();
		publication.committed();
	});
	entered.get_future().wait();
	CHECK(!publication.publish(stamp, [] { return true; }, [&] { ++delivered; }));
	release.set_value();
	writer.join();
	CHECK(!publication.publish(stamp, [] { return true; }, [&] { ++delivered; }));
	CHECK(publication.publish(publication.stamp(), [] { return true; }, [&] { ++delivered; }));
	publication.revoke();
	CHECK(!publication.publish(publication.stamp(), [] { return true; }, [&] { ++delivered; }));
	CHECK(delivered == 2);
	Section = "original partial response prefix";
	auto preview = Iv::RichPage();
	preview.part = true;
	auto block = Iv::RichPage::Block();
	block.kind = Iv::RichPage::BlockKind::Paragraph;
	block.text.text.text = u"observed prefix"_q;
	preview.blocks.push_back(block);
	auto full = preview;
	full.part = false;
	full.blocks.push_back(block);
	CHECK(DesktopRichPageExtends(preview, full));
	full.blocks.front().text.text.text = u"changed prefix"_q;
	CHECK(!DesktopRichPageExtends(preview, full));
	full = preview;
	CHECK(!DesktopRichPageExtends(preview, full));
	full.part = false;
	full.rtl = true;
	CHECK(!DesktopRichPageExtends(preview, full));
	full = preview;
	full.part = false;
	full.blocks.clear();
	CHECK(!DesktopRichPageExtends(preview, full));
	const auto blockWire = MTP_pageBlockParagraph(MTP_textPlain(MTP_string("observed")));
	const auto original = MTP_richMessage(MTP_flags(MTPDrichMessage::Flag::f_part),
		MTP_vector<MTPPageBlock>({blockWire}), MTP_vector<MTPPhoto>(), MTP_vector<MTPDocument>());
	const auto complete = MTP_richMessage(MTP_flags(MTPDrichMessage::Flags()),
		MTP_vector<MTPPageBlock>({blockWire, blockWire}), MTP_vector<MTPPhoto>(), MTP_vector<MTPDocument>());
	CHECK(DesktopRichWireExtends(original, complete));
	CHECK(!DesktopRichWireExtends(original, original));
	const auto changed = MTP_richMessage(MTP_flags(MTPDrichMessage::Flags()),
		MTP_vector<MTPPageBlock>({MTP_pageBlockParagraph(MTP_textPlain(MTP_string("different")))}),
		MTP_vector<MTPPhoto>(), MTP_vector<MTPDocument>());
	CHECK(!DesktopRichWireExtends(original, changed));
}

void RichCopyOpeningCases() {
	Section = "request-owned rich opening validation";
	auto preview = NativeMarkdownMessage();
	preview.id = MTP_int(100);
	preview.peer_id = MTP_peerChannel(MTP_long(45));
	preview.date = MTP_int(90);
	preview.message = MTP_string("");
	preview.flags = MTP_flags(MTPDmessage::Flag::f_rich_message);
	const auto block = MTP_pageBlockParagraph(MTP_textPlain(MTP_string("opening prefix")));
	preview.rich_message = MTP_richMessage(MTP_flags(MTPDrichMessage::Flag::f_part),
		MTP_vector<MTPPageBlock>({block}), MTP_vector<MTPPhoto>(), MTP_vector<MTPDocument>());
	const auto opening = preview.Build();
	auto full = preview;
	full.rich_message = MTP_richMessage(MTP_flags(MTPDrichMessage::Flags()),
		MTP_vector<MTPPageBlock>({block, block}), MTP_vector<MTPPhoto>(), MTP_vector<MTPDocument>());
	const auto response = [](QVector<MTPMessage> messages) {
		return MTP_messages_messages(MTP_vector<MTPMessage>(messages),
			MTP_vector<MTPForumTopic>(), MTP_vector<MTPChat>(), MTP_vector<MTPUser>());
	};
	CHECK(ValidateDesktopRichCopyResponse(opening, response({full.Build()})).has_value());
	CHECK(!ValidateDesktopRichCopyResponse(MTP_messageEmpty(MTP_flags(MTPDmessageEmpty::Flags()), MTP_int(0), MTPPeer()), response({full.Build()})));
	CHECK(!ValidateDesktopRichCopyResponse(opening, response({full.Build(), full.Build()})));
	CHECK(!ValidateDesktopRichCopyResponse(opening, response({opening})));
	for (auto i = 0; i != 5; ++i) {
		auto changed = full;
		switch (i) {
		case 0: changed.peer_id = MTP_peerChannel(MTP_long(999)); break;
		case 1: changed.date = MTP_int(91); break;
		case 2: changed.message = MTP_string("unverified replacement"); break;
		case 3: changed.id = MTP_int(101); break;
		case 4: changed.rich_message = MTP_richMessage(MTP_flags(MTPDrichMessage::Flags()),
			MTP_vector<MTPPageBlock>({MTP_pageBlockParagraph(MTP_textPlain(MTP_string("changed prefix")))}),
			MTP_vector<MTPPhoto>(), MTP_vector<MTPDocument>()); break;
		}
		CHECK(!ValidateDesktopRichCopyResponse(opening, response({changed.Build()})));
	}
	Section = "cached Copy admission boundary";
	auto parsedOpening = Iv::RichPage();
	parsedOpening.part = true;
	auto paragraph = Iv::RichPage::Block();
	paragraph.kind = Iv::RichPage::BlockKind::Paragraph;
	paragraph.text.text.text = u"opening prefix"_q;
	parsedOpening.blocks.push_back(paragraph);
	auto parsedFull = std::make_shared<Iv::RichPage>(parsedOpening);
	parsedFull->part = false;
	parsedFull->blocks.push_back(paragraph);
	CHECK(AdmitDesktopRichCopyPage(parsedOpening, *parsedFull, opening, full.Build(), parsedFull));
	CHECK(!AdmitDesktopRichCopyPage(parsedOpening, *parsedFull, std::nullopt, full.Build(), parsedFull));
	CHECK(!AdmitDesktopRichCopyPage(parsedOpening, *parsedFull, opening, std::nullopt, parsedFull));
	auto changedPage = std::make_shared<Iv::RichPage>(*parsedFull);
	changedPage->blocks.front().text.text.text = u"replacement prefix"_q;
	CHECK(!AdmitDesktopRichCopyPage(parsedOpening, *changedPage, opening, full.Build(), changedPage));
	CHECK(!AdmitDesktopRichCopyPage(parsedOpening, *changedPage, opening, full.Build(), parsedFull));
	auto newer = full;
	newer.flags = MTP_flags(MTPDmessage::Flag::f_rich_message | MTPDmessage::Flag::f_edit_date);
	newer.edit_date = MTP_int(91);
	CHECK(!AdmitDesktopRichCopyPage(parsedOpening, *parsedFull, opening, newer.Build(), parsedFull));

}

void DeliveryCases() {
	Section = "empty, absent and stale delivery boundary";
	auto gate = DesktopMarkdownPublication();
	auto clipboard = u"sentinel"_q;
	auto deliveries = 0;
	const auto deliver = [&](std::unique_ptr<QMimeData> data) {
		CHECK(data && data->hasText());
		clipboard = data->text();
		++deliveries;
	};
	CHECK(!DeliverDesktopMarkdownText(std::nullopt, deliver));
	CHECK(deliveries == 0 && clipboard == u"sentinel"_q);
	CHECK(gate.publish(gate.stamp(), [] { return true; }, [&] {
		CHECK(DeliverDesktopMarkdownText(QString(), deliver));
	}));
	CHECK(deliveries == 1 && clipboard.isEmpty());
	CHECK(DeliverDesktopMarkdownText(u"nonempty control"_q, deliver));
	CHECK(deliveries == 2 && clipboard == u"nonempty control"_q);
	const auto stamp = gate.stamp();
	gate.committed();
	CHECK(!gate.publish(stamp, [] { return true; }, [&] {
		CHECK(DeliverDesktopMarkdownText(QString(), deliver));
	}));
	CHECK(deliveries == 2 && clipboard == u"nonempty control"_q);
}

}

int main(int argc, char *argv[]) {
	QCoreApplication application(argc, argv);
	if (argc != 2) { return 2; }
	const auto root = QString::fromLocal8Bit(argv[1]);
	PublicationCases();
	RichCopyOpeningCases();
	DeliveryCases();
	AcknowledgementCases();
	ComposeCases(root + u"/compose"_q);
	RevocationCases(root + u"/revocation"_q);
	FileCases(root + u"/file"_q);
	PurgeCases(root + u"/purge"_q);
	RichHydrationCases(root + u"/rich"_q);
	Section = "UTF-8 provenance";
	CHECK(DecodeDesktopMarkdownSource(QByteArray()).has_value());
	CHECK(DecodeDesktopMarkdownSource("source") == u"source"_q);
	CHECK(!DecodeDesktopMarkdownSource(QByteArray("\xff", 1)));
	CHECK(DecodeDesktopMarkdownSource(QByteArray::fromHex("efbbbf61")) == QString(QChar(0xfeff)) + 'a');
	Section = "Android Markdown document recognition parity";
	auto descriptor = ArchiveMediaDescriptor{u"document:1"_q, u"document"_q,
		{{u"name"_q, u"source.txt"_q}, {u"mime"_q, u"text/plain"_q}}};
	CHECK(!IsDesktopMarkdownFile(descriptor));
	for (const auto suffix : { u".md"_q, u".markdown"_q, u".mkd"_q,
			u".mdown"_q, u".mkdn"_q, u".mdwn"_q }) {
		descriptor.attributes.insert(u"name"_q, u"source"_q + suffix);
		CHECK(IsDesktopMarkdownFile(descriptor));
		descriptor.attributes.insert(u"name"_q, u"SOURCE"_q + suffix.toUpper());
		CHECK(IsDesktopMarkdownFile(descriptor));
		descriptor.attributes.insert(u"name"_q, u"source"_q + suffix + u".txt"_q);
		CHECK(!IsDesktopMarkdownFile(descriptor));
	}
	descriptor.attributes.insert(u"name"_q, u"source.txt"_q);
	for (const auto mime : { u"text/markdown"_q, u"text/x-markdown"_q,
			u"text/x-web-markdown"_q }) {
		descriptor.attributes.insert(u"mime"_q, mime);
		CHECK(IsDesktopMarkdownFile(descriptor));
		descriptor.attributes.insert(u"mime"_q,
			u" \t"_q + mime.toUpper() + u" \t; charset=UTF-8; other=value"_q);
		CHECK(IsDesktopMarkdownFile(descriptor));
		descriptor.attributes.insert(u"mime"_q, mime + u"-other; charset=UTF-8"_q);
		CHECK(!IsDesktopMarkdownFile(descriptor));
	}
	descriptor.attributes.insert(u"mime"_q, u"text/plain; type=text/markdown"_q);
	CHECK(!IsDesktopMarkdownFile(descriptor));
	descriptor.attributes.insert(u"mime"_q, u";text/markdown"_q);
	CHECK(!IsDesktopMarkdownFile(descriptor));
	descriptor.attributes.insert(u"mime"_q, u"text/markdown"_q);
	descriptor.kind = u"photo"_q;
	CHECK(!IsDesktopMarkdownFile(descriptor));
	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}

#include "purple/purple_archive_journal.h"
#include "purple/purple_archive_stream.h"
#include "purple/purple_archive_files.h"
#include "purple/purple_archive_sql_maintenance.h"
#include "tests/archive_test_support.h"
#include <QtCore/QBuffer>
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDirIterator>
#include <QtCore/QTemporaryDir>
#include <atomic>

using namespace Purple;
using namespace ArchiveTest;

namespace {

DesktopArchiveOptions Options(const QString &root) {
	CHECK(DesktopArchiveDirectory(root + u"/profile"_q));
	auto result = DesktopArchiveOptions{root + u"/archives"_q, root + u"/profile"_q,
		{0, 123}, u"0"_q, QByteArray(256, 'k'), true};
	result.loginAuthority = QByteArray(32, 'a');
	result.loginAuthKeyId = QByteArray(16, '1');
	result.loginAuthKeyIds = result.loginAuthKeyId;
	return result;
}

ArchiveEvent MediaEvent(QString id = u"v1"_q, uint64_t order = 1) {
	auto event = Event(Empty(), id, order, u"retained expiring preimage"_q);
	event.content->media.push_back({u"document:555"_q, u"document"_q});
	return event;
}

void Streams() {
	Section = "real staging IO, out-of-order coverage and historical completion";
	auto temporary = QTemporaryDir();
	const auto options = Options(temporary.path());
	auto store = DesktopArchiveStore();
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	CHECK(store.Capture(MediaEvent()).retained);
	auto bytes = QByteArray(1024 * 1024 + 173, Qt::Uninitialized);
	for (auto i = 0; i != bytes.size(); ++i) {
		bytes[i] = char((i * 17 + i / 73) & 255);
	}
	const auto binding = store.StageStream(Key(), u"v1"_q, u"document:555"_q, bytes.size());
	CHECK(binding);
	if (!binding) {
		return;
	}
	const auto root = store.directory();
	{
		auto stream = DesktopArchiveStream(store, *binding);
		CHECK(stream.Open());
		CHECK(stream.Write(900000, bytes.mid(900000)) == ArchiveStagingStatus::Ready);
		CHECK(stream.Write(900000, bytes.mid(900000)) == ArchiveStagingStatus::Ready);
		CHECK(stream.Write(0, bytes.left(33)) == ArchiveStagingStatus::Ready);
		CHECK(stream.Write(1, QByteArray(1, char(bytes[1] ^ 1))) == ArchiveStagingStatus::Conflict);
		CHECK(!stream.Complete());
		CHECK(stream.header().coveredBytes == uint64_t(bytes.size() - 900000 + 33));
	}
	CHECK(store.Close());
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	{
		auto stream = DesktopArchiveStream(store, *binding);
		CHECK(stream.Open());
		CHECK(stream.header().coveredBytes == uint64_t(bytes.size() - 900000 + 33));
		CHECK(stream.Write(33, bytes.mid(33, 899967)) == ArchiveStagingStatus::Ready);
		CHECK(stream.header().coveredBytes == uint64_t(bytes.size()));
	}
	auto edited = MediaEvent(u"v2"_q, 3);
	edited.origin = ArchiveOrigin::AuthoritativeEdit;
	edited.content->text = u"new version remains independent"_q;
	CHECK(store.Capture(edited).retained);
	auto cursor = QString();
	auto more = false;
	CHECK(store.ResumeStreams(cursor, more));
	CHECK(!more);
	auto read = QByteArray();
	CHECK(store.ReadMedia(Key(), u"v1"_q, u"document:555"_q,
		[&](const QByteArray &part) { CHECK(part.size() <= kArchiveMediaChunkBytes); read += part; return true; }));
	CHECK(read == bytes);
	CHECK(!store.ReadMedia(Key(), u"v2"_q, u"document:555"_q, [](const QByteArray &) { return true; }));
	const auto slice = store.ReadSlice(Key(), {}, u"v1"_q);
	CHECK(slice && FindArchiveVersion(slice->state, Key(), u"v1"_q)->media.front().state == ArchiveMediaState::Complete);
	CHECK(slice && FindArchiveVersion(slice->state, Key(), u"v2"_q)->media.front().state == ArchiveMediaState::Missing);
	CHECK(store.Remove());
	CHECK(!QFileInfo::exists(root));
}

void EmptyAndFence() {
	Section = "zero stream, truncated input, wrong fences and revocation";
	for (const auto size : {0, 1, 262144, 262145}) {
		auto temporary = QTemporaryDir();
		auto options = Options(temporary.path());
		const auto alive = std::make_shared<std::atomic<bool>>(true);
		options.active = [alive] { return alive->load(); };
		auto store = DesktopArchiveStore();
		CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
		CHECK(store.Capture(MediaEvent()).retained);
		const auto binding = store.StageStream(Key(), u"v1"_q, u"document:555"_q, size);
		CHECK(binding);
		if (!binding) {
			continue;
		}
		auto stale = *binding;
		stale.source.epoch.generation++;
		CHECK(!store.StreamPart(stale, 0, QByteArray(size ? 1 : 0, 'x')));
		stale = *binding;
		stale.source.revision = u"wrong"_q;
		CHECK(!store.StreamPart(stale, 0, QByteArray(size ? 1 : 0, 'x')));
		if (size) {
			CHECK(!store.StreamPart(*binding, size, QByteArray(1, 'x')));
			CHECK(store.StreamPart(*binding, 0, QByteArray(size, 'x')));
		} else {
			CHECK(store.StreamPart(*binding, 0, {}));
		}
		auto read = QByteArray();
		CHECK(store.ReadMedia(Key(), u"v1"_q, u"document:555"_q,
			[&](const QByteArray &part) { read += part; return true; }));
		CHECK(read == QByteArray(size, 'x'));
		CHECK(store.StreamPart(*binding, 0, QByteArray(size, 'x')));
		if (size) {
			CHECK(!store.StreamPart(*binding, 0, QByteArray(1, 'y')));
		}
		alive->store(false);
		CHECK(!store.StreamPart(*binding, 0, {}));
		CHECK(!store.ReadMedia(Key(), u"v1"_q, u"document:555"_q, [](const QByteArray &) { return true; }));
		CHECK(store.Close());
		alive->store(true);
		CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
		CHECK(store.Remove());
	}
}

void DamagedStaging() {
	Section = "damaged or missing durable staging refuses without claiming ownership";
	for (const auto damage : {0, 1, 2}) {
		auto temporary = QTemporaryDir();
		auto store = DesktopArchiveStore();
		CHECK(store.Open(Options(temporary.path())) == DesktopArchiveStatus::Ready);
		CHECK(store.Capture(MediaEvent()).retained);
		const auto binding = store.StageStream(Key(), u"v1"_q, u"document:555"_q, 262145);
		CHECK(binding);
		if (!binding) {
			continue;
		}
		{
			auto stream = DesktopArchiveStream(store, *binding);
			CHECK(stream.Open());
			CHECK(stream.Write(0, QByteArray(262144, 'x')) == ArchiveStagingStatus::Ready);
		}
		const auto root = store.directory() + u"/staging/stream-"_q + ArchiveStagingBindingId(*binding, store.security());
		const auto files = QDir(root).entryList(QDir::Files | QDir::NoDotAndDotDot);
		if (damage == 2) {
			auto index = DesktopArchiveSqlite();
			CHECK(index.Open(root + u"/catalogue.sqlite"_q, false).ok);
			CHECK(index.Configure().ok);
			CHECK(index.Execute({ArchiveSqlStatus::Ready, {{u"BEGIN IMMEDIATE"_q},
				{u"UPDATE progress SET frame=? WHERE singleton=1"_q, {QByteArray("bad authenticated header")}, ArchiveSqlExpectation::OneChange},
				{u"COMMIT"_q}}}).committed);
		} else {
			const auto piece = std::find_if(files.begin(), files.end(), [](const auto &name) { return !name.startsWith(u"catalogue"_q); });
			CHECK(piece != files.end());
			if (piece != files.end()) {
				CHECK(damage ? DesktopArchiveRemoveFile(root + '/' + *piece)
					: DesktopArchiveWrite(root + '/' + *piece, QByteArray("bad ciphertext")));
			}
		}
		CHECK(!store.StreamPart(*binding, 0, QByteArray(262145, 'x')));
		const auto slice = store.ReadSlice(Key(), {}, u"v1"_q);
		CHECK(slice && FindArchiveVersion(slice->state, Key(), u"v1"_q)->media.front().state != ArchiveMediaState::Complete);
		CHECK(store.Remove());
	}
}

void StreamFailureRecovery() {
	Section = "real staging lock and final publication commit recovery";
	for (const auto commitFailure : {false, true}) {
		auto temporary = QTemporaryDir();
		const auto options = Options(temporary.path());
		auto store = DesktopArchiveStore();
		CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
		CHECK(store.Capture(MediaEvent()).retained);
		const auto bytes = QByteArray(262145, 'r');
		const auto binding = store.StageStream(Key(), u"v1"_q, u"document:555"_q, bytes.size());
		CHECK(binding);
		if (!binding) {
			continue;
		}
		{
			auto stream = DesktopArchiveStream(store, *binding);
			CHECK(stream.Open());
			if (!commitFailure) {
				const auto token = ArchiveStagingBindingId(*binding, store.security());
				auto locked = DesktopArchiveSqlite();
				CHECK(locked.Open(store.directory() + u"/staging/stream-"_q
					+ token + u"/catalogue.sqlite"_q, false).ok);
				CHECK(locked.Read({u"BEGIN IMMEDIATE"_q}).ok);
				CHECK(stream.Write(0, bytes) != ArchiveStagingStatus::Ready);
				CHECK(stream.header().coveredBytes == 0);
				CHECK(locked.Read({u"ROLLBACK"_q}).ok);
				CHECK(locked.Close());
			}
			CHECK(stream.Write(0, bytes) == ArchiveStagingStatus::Ready);
			CHECK(stream.header().coveredBytes == uint64_t(bytes.size()));
			if (commitFailure) {
				store.database().FailForTest(u"commit"_q);
				CHECK(!stream.Complete());
				const auto slice = store.ReadSlice(Key(), {}, u"v1"_q);
				const auto version = slice ? FindArchiveVersion(slice->state, Key(), u"v1"_q) : nullptr;
				CHECK(version && version->media.front().state != ArchiveMediaState::Complete);
			}
		}
		CHECK(store.Close());
		CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
		auto cursor = QString();
		auto more = false;
		CHECK(store.ResumeStreams(cursor, more));
		CHECK(!more);
		auto read = QByteArray();
		CHECK(store.ReadMedia(Key(), u"v1"_q, u"document:555"_q,
			[&](const QByteArray &part) { read += part; return true; }));
		CHECK(read == bytes);
		CHECK(store.Remove());
	}
}

void SmallFactLimitAndFallback() {
	Section = "exact small-write limit and expired placeholder with evicted live item";
	auto temporary = QTemporaryDir();
	auto store = DesktopArchiveStore();
	CHECK(store.Open(Options(temporary.path())) == DesktopArchiveStatus::Ready);
	const auto initial = MediaEvent();
	CHECK(store.Capture(initial).retained);
	const auto access = store.JournalAccess();
	CHECK(access);
	if (!access) {
		return;
	}
	{
		auto journal = DesktopArchiveJournal(*access);
		CHECK(journal.Open());
		auto large = MediaEvent(u"too-large"_q);
		large.content->text = QString(65536, 'x');
		CHECK(!journal.Append(large, 60 * 1024));
		CHECK(journal.failure() == u"outbox-version-too-large"_q);
	}
	{
		auto journal = DesktopArchiveJournal(*access);
		CHECK(journal.Open());
		auto fact = MediaEvent(u"evicted-expiry"_q);
		fact.content = ArchiveContent();
		fact.observation.context.insert(u"desktopLifecycleFallback"_q, true);
		fact.observation.context.insert(u"desktopLivePreimage"_q, true);
		fact.observation.context.insert(u"desktopLifecycle"_q, int(ArchiveMediaTransition::ExpiredReplacement));
		CHECK(journal.Append(fact, 60 * 1024));
		CHECK(journal.Drain(store));
		const auto slice = store.ReadSlice(Key());
		const auto message = slice ? FindArchiveMessage(slice->state, Key()) : nullptr;
		CHECK(message && message->deleted && message->versions.size() == 1);
		CHECK(message && message->versions.back().content == *initial.content);
	}
	CHECK(store.Remove());
}

void Lifecycle() {
	Section = "durable lifecycle bridge, duplicates, ordinary reads and body-only TTL";
	for (const auto transition : {ArchiveMediaTransition::Expiry, ArchiveMediaTransition::Consumption,
		ArchiveMediaTransition::ExpiredReplacement}) {
		auto temporary = QTemporaryDir();
		auto store = DesktopArchiveStore();
		CHECK(store.Open(Options(temporary.path())) == DesktopArchiveStatus::Ready);
		const auto preimage = MediaEvent();
		CHECK(store.Capture(preimage).retained);
		const auto access = store.JournalAccess();
		CHECK(access);
		if (!access) {
			continue;
		}
		auto fact = preimage;
		fact.id = u"lifecycle-preimage"_q;
		fact.order = 0;
		fact.generation = access->generation;
		fact.observation.context.insert(u"desktopLivePreimage"_q, true);
		fact.observation.context.insert(u"desktopLifecycle"_q, int(transition));
		fact.observation.context.insert(u"desktopLifecycleObservation"_q, preimage.id);
		fact.observation.context.insert(u"desktopLifecycleHash"_q, ArchiveContentHash(*preimage.content));
		{
			auto journal = DesktopArchiveJournal(*access);
			CHECK(journal.Open());
			CHECK(journal.Append(fact));
		}
		auto journal = DesktopArchiveJournal(*access);
		CHECK(journal.Open());
		CHECK(journal.Drain(store));
		CHECK(journal.Append(fact));
		CHECK(journal.Drain(store));
		const auto slice = store.ReadSlice(Key());
		const auto message = slice ? FindArchiveMessage(slice->state, Key()) : nullptr;
		CHECK(message && message->deleted && message->versions.size() == 1);
		CHECK(message && message->deletionOrigin == (transition == ArchiveMediaTransition::Consumption
			? ArchiveOrigin::MediaConsumption : ArchiveOrigin::MediaExpiry));
		CHECK(message && message->versions.front().content.text == preimage.content->text);
		CHECK(store.Remove());
	}
	for (const auto ordinary : {false, true}) {
		auto temporary = QTemporaryDir();
		auto store = DesktopArchiveStore();
		CHECK(store.Open(Options(temporary.path())) == DesktopArchiveStatus::Ready);
		const auto initial = ordinary ? MediaEvent() : Event(Empty(), u"body"_q, 1);
		CHECK(store.Capture(initial).retained);
		const auto slice = store.ReadSlice(Key());
		CHECK(slice);
		if (ordinary && slice) {
			const auto source = ArchiveMediaSource{store.epoch(), Key(), initial.id, initial.id,
				ArchiveContentHash(*initial.content), u"document:555"_q, {0, 0}};
			for (const auto transition : {ArchiveMediaTransition::Consumption, ArchiveMediaTransition::CacheEviction}) {
				const auto plan = PlanArchiveMediaLifecycle(slice->state, store.epoch(),
					{source, ArchiveMediaLifetime::Ordinary, transition, ArchiveAvailableBytes::Complete, false, u"read"_q, 2, 100});
				CHECK(plan.transaction.status == ArchivePlanStatus::NoChange);
				CHECK(!plan.event);
			}
		} else {
			auto fact = initial;
			fact.id = u"body-ttl"_q;
			fact.order = 2;
			fact.observation.context.insert(u"desktopWholeMessageTtl"_q, true);
			fact.observation.context.insert(u"desktopLifecycle"_q, int(ArchiveMediaTransition::Expiry));
			CHECK(store.CaptureLifecycle(fact, store.epoch()));
			const auto after = store.ReadSlice(Key());
			CHECK(after && FindArchiveMessage(after->state, Key())->deleted);
		}
		CHECK(store.Remove());
	}
}

}

int main(int argc, char **argv) {
	const auto application = QCoreApplication(argc, argv);
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	Streams();
	EmptyAndFence();
	DamagedStaging();
	StreamFailureRecovery();
	SmallFactLimitAndFallback();
	Lifecycle();
	std::printf("Desktop archive lifecycle: %d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}

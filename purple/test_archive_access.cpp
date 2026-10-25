#include "purple/purple_archive_media_loader.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "base/bytes.h"
#include "base/thread_safe_wrap.h"
#include "purple/purple_account_files.h"
#include "purple/purple_archive_files.h"
#include "purple/purple_archive_sql_maintenance.h"
#include "purple/purple_desktop_archive_store.h"
#include "storage/cache/storage_cache_types.h"
#include "storage/cache/storage_cache_database.h"
#include "tests/archive_test_support.h"
#include <QtCore/QBuffer>
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <deque>
#include <set>
#include <thread>
#include <QtCore/QThread>

#define private public
#include "media/streaming/media_streaming_reader.h"
#undef private

#include "archive-reader-slices.inc"

using namespace Purple;
using namespace ArchiveTest;

namespace Purple::ServiceProbe {

struct CallbackSession : base::has_weak_ptr {
	AccountKey key{0, 123};
};

struct CallbackWorker {
	DesktopArchiveStore *store = nullptr;
	bool accept = true;
	bool stopped = false;
	std::deque<std::function<void(DesktopArchiveStore&)>> tasks;
	std::vector<std::shared_ptr<std::atomic<bool>>> fences;
	bool Post(std::function<void(DesktopArchiveStore&)> task, bool cleanup = false) {
		if (stopped || (!accept && !cleanup)) {
			return false;
		}
		tasks.push_back(std::move(task));
		return true;
	}
	void RevokeOnStop(std::shared_ptr<std::atomic<bool>> flag) {
		fences.push_back(std::move(flag));
	}
	void Run() {
		while (!tasks.empty()) {
			auto task = std::move(tasks.front());
			tasks.pop_front();
			task(*store);
		}
	}
	void Stop() {
		stopped = true;
		for (const auto &flag : fences) {
			flag->store(false);
		}
		tasks.clear();
	}
};

struct CallbackJournal {
	struct Access {
		std::function<bool()> current = [] { return true; };
	};
	Access access() { return {}; }
	bool Drain(DesktopArchiveStore&) { return true; }
};

struct CallbackValue {
	std::shared_ptr<CallbackWorker> worker;
	std::shared_ptr<CallbackJournal> journal;
};

CallbackValue Value;
std::set<CallbackSession*> Retired;
std::deque<std::function<void()>> Main;

AccountKey SessionAccountKey(not_null<CallbackSession*> session) {
	return session->key;
}

CallbackValue &For(not_null<CallbackSession*>) {
	return Value;
}

template <typename Callback>
void CallbackOnMain(Callback callback) {
	Main.push_back(std::move(callback));
}

void Flush() {
	for (auto i = 0; i != 64; ++i) {
		Value.worker->Run();
		if (Main.empty() && Value.worker->tasks.empty()) {
			return;
		}
		while (!Main.empty()) {
			auto task = std::move(Main.front());
			Main.pop_front();
			task();
		}
	}
	CHECK(false);
}

#include "archive-media-callback.inc"

}

namespace {

DesktopArchiveOptions Options(const QString &root) {
	CHECK(DesktopArchiveDirectory(root + u"/profile"_q));
	auto options = DesktopArchiveOptions{root + u"/archives"_q, root + u"/profile"_q,
		{0, 123}, u"0"_q, QByteArray(256, 'k'), true};
	options.loginAuthority = QByteArray(32, 'a');
	options.loginAuthKeyId = options.loginAuthKeyIds = QByteArray(16, '1');
	return options;
}

QString Identity(const QString &version) {
	return u"attachment:"_q + version;
}

void Owned(DesktopArchiveStore &store, const QByteArray &bytes, QString version, uint64_t order) {
	auto event = Event(Empty(), version, order, version,
		order == 1 ? ArchiveOrigin::Baseline : ArchiveOrigin::AuthoritativeEdit);
	event.content->media.push_back({Identity(version), u"document"_q});
	CHECK(store.Capture(event).retained);
	auto buffer = QBuffer();
	buffer.setData(bytes);
	CHECK(buffer.open(QIODevice::ReadOnly));
	const auto hash = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
	CHECK(store.CopyMedia(Key(), version, Identity(version), buffer, {hash, uint64_t(bytes.size())}));
}

size_t NativeLeases(DesktopArchiveStore &store) {
	const auto result = store.database().Read(ArchiveSqlReadPage(ArchiveRecordKind::Lease, 128));
	CHECK(result.ok);
	return result.rows.size();
}

std::optional<ArchiveMediaRequest> MediaRequest(DesktopArchiveStore &store,
	const ArchiveMessageKey &key, const QString &version, const QString &identity) {
	const auto page = store.Versions(key, 128);
	if (page.status != DesktopArchiveStatus::Ready) {
		return {};
	}
	const auto found = std::find_if(page.versions.begin(), page.versions.end(),
		[&](const auto &value) { return value.version.id == version; });
	if (found == page.versions.end()) {
		return {};
	}
	const auto ref = std::find_if(found->version.media.begin(), found->version.media.end(),
		[&](const auto &value) { return value.identity == identity; });
	if (ref == found->version.media.end()) {
		return {};
	}
	return ArchiveMediaRequest{{page.epoch, key, version, found->version.revision,
		ArchiveContentHash(found->version.content), identity, page.generation}, ref->blobId};
}

std::optional<DesktopArchiveMediaHandle> OpenOwned(DesktopArchiveStore &store,
	const ArchiveMessageKey &key, const QString &version, const QString &identity) {
	const auto request = MediaRequest(store, key, version, identity);
	return request ? store.OpenMedia(*request) : std::nullopt;
}

void NativeAccess() {
	Section = "native owned historical range, export and capability fences";
	auto temporary = QTemporaryDir();
	const auto options = Options(temporary.path());
	auto store = DesktopArchiveStore();
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	if (!store.Current()) {
		return;
	}
	auto order = uint64_t(0);
	for (const auto size : {0, 1, 1024 * 1024, 1024 * 1024 + 1, 3 * 1024 * 1024 + 7}) {
		auto bytes = QByteArray(size, '\0');
		for (auto i = 0; i != size; ++i) {
			bytes[i] = char((i * 17 + i / 97) % 251);
		}
		const auto version = u"owned-"_q + QString::number(size);
		Owned(store, bytes, version, ++order);
		CHECK(!OpenOwned(store, Key({0, 456}), version, Identity(version)));
		CHECK(!OpenOwned(store, Key(), version, u"wrong"_q));
		auto handle = OpenOwned(store, Key(), version, Identity(version));
		CHECK(handle && store.MediaCurrent(*handle));
		if (!handle) {
			continue;
		}
		CHECK(store.MediaRange(*handle, size, 0) == std::optional(QByteArray()));
		CHECK(!store.MediaRange(*handle, uint64_t(size) + 1, 0));
		CHECK(!store.MediaRange(*handle, 0, kArchiveMediaChunkBytes + 1));
		for (const auto offset : {uint64_t(0), uint64_t(size / 2), uint64_t(size)}) {
			const auto length = uint32_t(std::min(uint64_t(kArchiveMediaChunkBytes), uint64_t(size) - offset));
			CHECK(store.MediaRange(*handle, offset, length) == std::optional(bytes.mid(offset, length)));
		}
		auto forged = *handle;
		++forged.source.key.messageId;
		CHECK(!store.MediaCurrent(forged));
		forged = *handle;
		forged.source.revision += 'x';
		CHECK(!store.MediaRange(forged, 0, 0));
		CHECK(store.MediaCurrent(*handle));
		const auto path = temporary.path() + u"/export"_q;
		CHECK(store.ExportMedia(*handle, path));
		auto file = QFile(path);
		CHECK(file.open(QIODevice::ReadOnly));
		CHECK(file.readAll() == bytes);
		file.close();
		const auto inside = store.directory() + u"/forbidden"_q;
		CHECK(!store.ExportMedia(*handle, inside));
		CHECK(store.ReleaseMedia(*handle));
		CHECK(!handle->active->load());
		CHECK(!store.MediaRange(*handle, 0, 0));
		CHECK(!store.ReleaseMedia(*handle));
	}
	const auto old = OpenOwned(store, Key(), u"owned-1"_q, Identity(u"owned-1"_q));
	const auto viewed = MediaRequest(store, Key(), u"owned-1"_q, Identity(u"owned-1"_q));
	CHECK(viewed);
	CHECK(old && store.MediaRange(*old, 0, 1) == std::optional(QByteArray(1, '\0')));
	if (!old) {
		return;
	}
	Owned(store, "new head", u"new-head"_q, ++order);
	const auto historical = store.OpenMedia(*viewed);
	CHECK(historical && store.ReleaseMedia(*historical));
	auto mismatched = *viewed;
	++mismatched.source.generation.chat;
	CHECK(!store.OpenMedia(mismatched));
	mismatched = *viewed;
	mismatched.blob = QString(64, 'f');
	CHECK(!store.OpenMedia(mismatched));
	CHECK(store.MediaCurrent(*old));
	CHECK(store.MediaRange(*old, 0, 1) == std::optional(QByteArray(1, '\0')));
	auto readers = std::vector<DesktopArchiveMediaHandle>{*old};
	for (auto i = 1; i != 16; ++i) {
		const auto next = OpenOwned(store, Key(), u"owned-1"_q, Identity(u"owned-1"_q));
		CHECK(next);
		if (next) {
			readers.push_back(*next);
		}
	}
	CHECK(!OpenOwned(store, Key(), u"new-head"_q, Identity(u"new-head"_q)));
	for (const auto &reader : readers) {
		CHECK(store.ReleaseMedia(reader));
	}
	CHECK(NativeLeases(store) == 0);
	const auto retry = OpenOwned(store, Key(), u"new-head"_q, Identity(u"new-head"_q));
	CHECK(retry);
	if (retry) {
		CHECK(store.database().BeginRead());
		CHECK(!store.ReleaseMedia(*retry));
		CHECK(store.database().EndRead());
		CHECK(store.ReleaseMedia(*retry));
		CHECK(NativeLeases(store) == 0);
	}
	const auto current = OpenOwned(store, Key(), u"new-head"_q, Identity(u"new-head"_q));
	CHECK(current);
	if (!current) {
		return;
	}
	const auto slice = store.ReadSlice(Key());
	CHECK(slice);
	auto header = slice->state;
	header.messages.clear(); header.blobs.clear(); header.jobs.clear(); header.leases.clear(); header.chatGenerations.clear();
	const auto start = BeginArchiveSqlPurge(header, Purge(header, ArchivePurgeKind::EditHistory),
		{}, u"test-access-purge"_q, store.security());
	CHECK(start.program && store.Execute(start.program));
	CHECK(store.Versions(Key()).status != DesktopArchiveStatus::Ready);
	CHECK(!current->active->load());
	CHECK(!store.MediaRange(*current, 0, 1));
	CHECK(!store.ReleaseMedia(*current));
	const auto leaseToken = ArchiveSqlToken(Key().account, ArchiveIndexKind::Lease,
		current->lease.toUtf8(), store.security());
	const auto pending = store.database().Read({
		u"SELECT envelope FROM leases WHERE lease_id=?"_q, {leaseToken}, ArchiveSqlExpectation::OneRow});
	CHECK(pending.ok && pending.rows.size() == 1);
	if (pending.ok && pending.rows.size() == 1) {
		const auto plain = OpenArchiveEnvelope({Key().account, store.security().storeId,
			ArchiveRecordKind::Lease, leaseToken.toUtf8()}, store.security().keyId,
			std::get<QByteArray>(pending.rows[0][0]), store.security().crypto);
		const auto decoded = plain ? DecodeArchiveRow(plain.bytes) : ArchiveRowDecodeResult();
		CHECK(decoded.row && std::get<ArchiveReaderLease>(decoded.row->value).id == current->lease);
	}
	CHECK(store.Remove());
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	Owned(store, QByteArray(1, '\0'), u"owned-1"_q, 1);
	CHECK(!store.OpenMedia(*viewed));
	CHECK(!store.MediaCurrent(*old));
	CHECK(store.Versions(Key()).versions.size() == 1);
	CHECK(store.Remove());
}

void NativeFailures() {
	Section = "authenticated chunks and atomic export refusal";
	auto temporary = QTemporaryDir();
	auto store = DesktopArchiveStore();
	CHECK(store.Open(Options(temporary.path())) == DesktopArchiveStatus::Ready);
	if (!store.Current()) {
		return;
	}
	Owned(store, "retained attachment", u"owned"_q, 1);
	const auto handle = OpenOwned(store, Key(), u"owned"_q, Identity(u"owned"_q));
	CHECK(handle);
	if (!handle) {
		return;
	}
	const auto alias = temporary.path() + u"/archive-alias"_q;
	CHECK(QFile::link(store.directory(), alias));
	CHECK(!store.ExportMedia(*handle, alias + u"/forbidden"_q));
	const auto path = temporary.path() + u"/destination"_q;
	CHECK(DesktopArchiveWrite(path, "keep destination"));
	const auto token = ArchiveSqlToken(Key().account, ArchiveIndexKind::Blob, handle->blob.toUtf8(), store.security());
	const auto directory = store.directory() + u"/blobs/"_q + token;
	const auto chunk = directory + u"/00000000000000000000.chunk"_q;
	const auto original = DesktopArchiveRead(chunk, 2 * kArchiveMediaChunkBytes);
	CHECK(original);
	auto damaged = *original;
	damaged[damaged.size() / 2] ^= 1;
	CHECK(DesktopArchiveWrite(chunk, damaged));
	CHECK(!store.MediaRange(*handle, 0, 1));
	CHECK(!store.ExportMedia(*handle, path));
	CHECK(DesktopArchiveRead(path, 1024) == std::optional(QByteArray("keep destination")));
	CHECK(DesktopArchiveWrite(chunk, *original));
	const auto manifest = directory + u"/manifest"_q;
	const auto saved = DesktopArchiveRead(manifest, 8192);
	CHECK(saved && DesktopArchiveWrite(manifest, "invalid"));
	CHECK(!store.MediaRange(*handle, 0, 1));
	CHECK(!store.ExportMedia(*handle, path));
	CHECK(DesktopArchiveWrite(manifest, *saved));
	auto calls = 0;
	CHECK(!DesktopArchiveExportMedia(store.directory(), Key().account, store.security(),
		{handle->blob, handle->size}, path, [&] { return ++calls < 4; }));
	CHECK(DesktopArchiveRead(path, 1024) == std::optional(QByteArray("keep destination")));
	CHECK(QFile::remove(chunk));
	CHECK(!store.ExportMedia(*handle, path));
	CHECK(DesktopArchiveRead(path, 1024) == std::optional(QByteArray("keep destination")));
	CHECK(store.ReleaseMedia(*handle));
	Owned(store, QByteArray(3 * kArchiveMediaChunkBytes + 7, 'p'), u"large"_q, 2);
	const auto large = OpenOwned(store, Key(), u"large"_q, Identity(u"large"_q));
	CHECK(large);
	if (large) {
		calls = 0;
		CHECK(!DesktopArchiveExportMedia(store.directory(), Key().account, store.security(),
			{large->blob, large->size}, path, [&] { return ++calls < 7; }));
		CHECK(calls == 7);
		CHECK(DesktopArchiveRead(path, 1024) == std::optional(QByteArray("keep destination")));
		CHECK(store.ReleaseMedia(*large));
	}
	CHECK(store.Remove());
}

void RevisionRevocation() {
	Section = "same-version revision and close revoke media delivery";
	auto temporary = QTemporaryDir();
	auto store = DesktopArchiveStore();
	CHECK(store.Open(Options(temporary.path())) == DesktopArchiveStatus::Ready);
	if (!store.Current()) {
		return;
	}
	Owned(store, "immutable bytes", u"v1"_q, 1);
	const auto handle = OpenOwned(store, Key(), u"v1"_q, Identity(u"v1"_q));
	const auto viewed = MediaRequest(store, Key(), u"v1"_q, Identity(u"v1"_q));
	CHECK(handle);
	if (!handle) {
		return;
	}
	auto next = Event(Empty(), u"v2"_q, 2, u"v1"_q, ArchiveOrigin::AuthoritativeEdit);
	next.content->media.push_back({Identity(u"v1"_q), u"document"_q});
	CHECK(store.Capture(next).retained);
	CHECK(viewed && !store.OpenMedia(*viewed));
	CHECK(!handle->active->load());
	CHECK(!store.MediaRange(*handle, 0, 1));
	CHECK(store.ReleaseMedia(*handle));
	const auto newHandle = OpenOwned(store, Key(), u"v1"_q, Identity(u"v1"_q));
	CHECK(newHandle);
	CHECK(store.Close());
	CHECK(newHandle && !newHandle->active->load());
	CHECK(!store.MediaRange(*newHandle, 0, 1));
	CHECK(store.Open(Options(temporary.path())) == DesktopArchiveStatus::Ready);
	CHECK(NativeLeases(store) == 0);
	CHECK(!store.MediaCurrent(*newHandle));
	CHECK(store.Remove());
}

void EndpointAndLoader() {
	Section = "production async endpoint and local loader stale completions";
	auto handle = DesktopArchiveMediaHandle();
	handle.size = 1024 * 1024;
	handle.active = std::make_shared<std::atomic<bool>>(true);
	auto valid = true;
	auto releases = 0;
	auto reads = std::vector<std::pair<uint32_t, ArchiveMediaAccess::ReadDone>>();
	auto saveDone = ArchiveMediaAccess::SaveDone();
	const auto access = std::make_shared<ArchiveMediaAccess>(handle,
		[&](uint64_t, uint32_t size, auto done) { reads.emplace_back(size, std::move(done)); },
		[&](QString, auto done) { saveDone = std::move(done); }, [&] { ++releases; }, [&] { return valid; });
	CHECK(ArchiveMediaLoader::CanOpen(access->size()));
	CHECK(!ArchiveMediaLoader::CanOpen(0));
	CHECK(!ArchiveMediaLoader::CanOpen(uint64_t(1) << 32));
	const auto maximum = uint64_t(std::numeric_limits<uint32_t>::max()) - 16 * 1024 * 1024;
	CHECK(ArchiveMediaLoader::CanOpen(maximum));
	CHECK(!ArchiveMediaLoader::CanOpen(maximum + 1));
	auto parts = std::vector<Media::Streaming::LoadedPart>();
	auto loader = std::make_unique<ArchiveMediaLoader>(access);
	CHECK(!loader->baseCacheKey().valid());
	auto life = loader->parts() | rpl::on_next([&](auto part) { parts.push_back(std::move(part)); });
	loader->load(0);
	loader->cancel(0);
	loader->load(0);
	CHECK(reads.size() == 2);
	reads[0].second(QByteArray(reads[0].first, 'x'));
	QCoreApplication::processEvents();
	CHECK(parts.empty());
	reads[1].second(QByteArray(reads[1].first, 'y'));
	CHECK(parts.empty());
	QCoreApplication::processEvents();
	CHECK(parts.size() == 1 && parts[0].bytes == QByteArray(reads[1].first, 'y'));
	loader->load(Media::Streaming::Loader::kPartSize);
	loader->stop();
	reads[2].second(QByteArray(reads[2].first, 'z'));
	QCoreApplication::processEvents();
	CHECK(parts.size() == 1);
	loader->load(2 * Media::Streaming::Loader::kPartSize);
	loader.reset();
	reads[3].second(QByteArray(reads[3].first, 'z'));
	QCoreApplication::processEvents();
	CHECK(parts.size() == 1);
	auto delivered = true;
	access->read(0, 1, [&](auto bytes) { delivered = bool(bytes); });
	valid = false;
	reads.back().second(QByteArray(1, 'x'));
	CHECK(!delivered && releases == 1 && !access->current());
	access->close();
	CHECK(releases == 1);
	auto saveHandle = handle;
	saveHandle.active = std::make_shared<std::atomic<bool>>(true);
	auto saveReleases = 0;
	auto saveCurrent = true;
	const auto saveAccess = std::make_shared<ArchiveMediaAccess>(saveHandle, ArchiveMediaAccess::Reader(),
		[&](QString, auto done) { saveDone = std::move(done); }, [&] { ++saveReleases; }, [&] { return saveCurrent; });
	auto saved = true;
	saveAccess->save(u"explicit destination"_q, [&](bool result) { saved = result; });
	CHECK(bool(saveDone));
	saveCurrent = false;
	saveDone(true);
	CHECK(!saved && saveReleases == 1);
	auto badHandle = handle;
	badHandle.active = std::make_shared<std::atomic<bool>>(true);
	auto badRead = ArchiveMediaAccess::ReadDone();
	auto badReleases = 0;
	const auto bad = std::make_shared<ArchiveMediaAccess>(badHandle,
		[&](uint64_t, uint32_t, auto done) { badRead = std::move(done); },
		ArchiveMediaAccess::Saver(), [&] { ++badReleases; }, [] { return true; });
	auto refused = false;
	bad->read(0, 1, [&](auto bytes) { refused = !bytes; });
	badRead(QByteArray(2, 'x'));
	CHECK(refused && badReleases == 1 && !bad->current());
	auto boundedHandle = handle;
	boundedHandle.size = 2 * 1024 * 1024;
	boundedHandle.active = std::make_shared<std::atomic<bool>>(true);
	auto queued = std::vector<ArchiveMediaAccess::ReadDone>();
	const auto bounded = std::make_shared<ArchiveMediaAccess>(boundedHandle,
		[&](uint64_t, uint32_t, auto done) { queued.push_back(std::move(done)); },
		ArchiveMediaAccess::Saver(), [] {}, [] { return true; });
	auto limited = ArchiveMediaLoader(bounded);
	auto failed = 0;
	auto ready = 0;
	auto boundedLife = limited.parts() | rpl::on_next([&](auto part) {
		part.offset == Media::Streaming::LoadedPart::kFailedOffset ? ++failed : ++ready;
	});
	for (auto i = 0; i != 9; ++i) {
		limited.load(i * Media::Streaming::Loader::kPartSize);
	}
	CHECK(queued.size() == 8 && failed == 0);
	const auto firstReads = queued;
	for (const auto &done : firstReads) {
		done(QByteArray(Media::Streaming::Loader::kPartSize, 'x'));
	}
	QCoreApplication::processEvents();
	CHECK(ready == 8 && queued.size() == 9 && failed == 0);
	queued.back()(QByteArray(Media::Streaming::Loader::kPartSize, 'y'));
	QCoreApplication::processEvents();
	CHECK(ready == 9 && failed == 0);

}

void ServiceCallbacks() {
	using namespace Purple::ServiceProbe;
	Section = "exact production service callbacks with controlled executor scheduling";
	auto temporary = QTemporaryDir();
	auto store = DesktopArchiveStore();
	CHECK(store.Open(Options(temporary.path())) == DesktopArchiveStatus::Ready);
	if (!store.Current()) {
		return;
	}
	Owned(store, "service-owned bytes", u"service"_q, 1);
	auto request = *MediaRequest(store, Key(), u"service"_q, Identity(u"service"_q));
	Value = {std::make_shared<CallbackWorker>(), std::make_shared<CallbackJournal>()};
	Value.worker->store = &store;
	auto session = CallbackSession();
	const auto leases = [&] { return NativeLeases(store); };
	for (const auto wrongAccount : {false, true}) {
		session.key = wrongAccount ? AccountKey{0, 456} : AccountKey{0, 123};
		auto called = false;
		OpenArchiveMedia(&session, request, [wrongAccount] { return wrongAccount; },
			[&](auto opened) { called = true; CHECK(!opened); });
		CHECK(called && Value.worker->tasks.empty() && leases() == 0);
	}
	session.key = {0, 123};
	for (auto cause = 0; cause != 3; ++cause) {
		auto source = AccountKey{0, 123};
		auto mode = AccountWorkMode::Own;
		const auto current = [&] { return source == AccountKey{0, 123} && mode == AccountWorkMode::Own; };
		auto access = std::shared_ptr<ArchiveMediaAccess>();
		OpenArchiveMedia(&session, request, current,
			[&](auto opened) { access = std::move(opened); });
		Flush();
		CHECK(access && access->current() && leases() == 1);
		auto delivered = true;
		access->read(0, 1, [&](auto bytes) { delivered = bool(bytes); });
		if (cause == 0) {
			Retired.insert(&session);
		} else if (cause == 1) {
			source = {0, 456};
		} else {
			mode = AccountWorkMode::Stock;
		}
		Flush();
		CHECK(!delivered && !access->current() && leases() == 0);
		Retired.clear();
	}
	auto first = std::shared_ptr<ArchiveMediaAccess>();
	auto second = std::shared_ptr<ArchiveMediaAccess>();
	const auto open = [&](auto &result) {
		OpenArchiveMedia(&session, request, [] { return true; },
			[&result](auto opened) { result = std::move(opened); });
	};
	open(first);
	Flush();
	auto stale = true;
	first->read(0, 1, [&](auto bytes) { stale = bool(bytes); });
	first->close();
	open(second);
	Flush();
	CHECK(!stale && second && second->current() && leases() == 1);
	second->close();
	Flush();
	CHECK(leases() == 0);
	Value.worker->accept = false;
	open(first);
	CHECK(!first && Value.worker->tasks.empty() && leases() == 0);
	Value.worker->accept = true;
	open(first);
	Flush();
	Value.worker->accept = false;
	auto refused = false;
	first->read(0, 1, [&](auto bytes) { refused = !bytes; });
	Flush();
	CHECK(refused && !first->current() && leases() == 0);
	Value.worker->accept = true;
	auto dying = std::make_unique<CallbackSession>();
	auto deadDelivered = true;
	OpenArchiveMedia(dying.get(), request, [] { return true; },
		[&](auto access) { deadDelivered = bool(access); });
	Value.worker->Run();
	dying.reset();
	Flush();
	CHECK(!deadDelivered && leases() == 0);
	Retired.insert(&session);
	open(first);
	CHECK(!first && Value.worker->tasks.empty());
	Retired.clear();
	open(first);
	auto changed = Event(Empty(), u"service-r2"_q, 2, u"service"_q, ArchiveOrigin::AuthoritativeEdit);
	changed.content->media.push_back({Identity(u"service"_q), u"document"_q});
	CHECK(store.Capture(changed).retained);
	Flush();
	CHECK(!first && leases() == 0);
	request = *MediaRequest(store, Key(), u"service"_q, Identity(u"service"_q));
	open(first);
	Value.worker->Run();
	Value.worker->Stop();
	const auto root = store.directory();
	CHECK(store.Remove());
	Flush();
	CHECK(!first && !QFileInfo::exists(root));
	Value = {};
}

void ReaderMemory() {
	Section = "exact native reader uncached eviction across long playback and seeks";
	using Reader = Media::Streaming::Reader;
	constexpr auto part = uint32_t(Media::Streaming::Loader::kPartSize);
	constexpr auto sliceBytes = uint32_t(64) * part;
	for (const auto bounded : {false, true}) {
		auto slices = Reader::Slices(5 * sliceBytes, false, bounded);
		auto buffer = QByteArray(part, '\0');
		for (const auto reverse : {false, true}) {
			for (auto step = 0; step != 5; ++step) {
				const auto slice = reverse ? 4 - step : step;
				slices.fill(slice * sliceBytes, bytes::make_span(buffer.data(), buffer.size()));
				for (auto index = uint32_t(0); index != 64; ++index) {
					if (!slices._data[slice].parts.contains(index * part)) {
						slices.processPart(slice * sliceBytes + index * part, QByteArray(part, char(slice + 1)));
					}
				}
				const auto filled = slices.fill(slice * sliceBytes,
					bytes::make_span(buffer.data(), buffer.size()));
				CHECK(filled.state == Reader::FillState::Success);
				CHECK(buffer == QByteArray(part, char(slice + 1)));
				CHECK(filled.toCache.number < 0);
				auto retained = uint64_t(0);
				for (const auto &value : slices._data) {
					for (const auto &[offset, bytes] : value.parts) {
						retained += bytes.size();
					}
				}
				if (bounded) {
					CHECK(retained <= 2 * uint64_t(sliceBytes));
				} else if (reverse) {
					CHECK(retained == 5 * uint64_t(sliceBytes));
				}
			}
		}
	}
	const auto maximum = uint32_t(std::numeric_limits<uint32_t>::max() - 16 * 1024 * 1024);
	auto edge = Reader::Slices(maximum, false, true);
	const auto offset = (maximum - 1) / part * part;
	const auto length = maximum - offset;
	auto buffer = QByteArray(length, '\0');
	edge.fill(offset, bytes::make_span(buffer.data(), buffer.size()));
	edge.processPart(offset, QByteArray(length, 'e'));
	CHECK(edge.fill(offset, bytes::make_span(buffer.data(), buffer.size())).state == Reader::FillState::Success);
	CHECK(buffer == QByteArray(length, 'e'));
}

void NativeDemandAndThread() {
	Section = "native slice demand, overlapping seeks, late delivery and file-thread loader calls";
	using Reader = Media::Streaming::Reader;
	constexpr auto part = uint32_t(Media::Streaming::Loader::kPartSize);
	constexpr auto sliceBytes = uint32_t(64) * part;
	auto slices = Reader::Slices(5 * sliceBytes, false, true);
	auto handle = DesktopArchiveMediaHandle();
	handle.size = 5 * sliceBytes;
	handle.active = std::make_shared<std::atomic<bool>>(true);
	struct Read {
		uint64_t offset;
		uint32_t length;
		ArchiveMediaAccess::ReadDone done;
	};
	auto reads = std::vector<Read>();
	auto access = std::make_shared<ArchiveMediaAccess>(handle,
		[&](uint64_t offset, uint32_t length, auto done) {
			CHECK(QThread::currentThread() == QCoreApplication::instance()->thread());
			reads.push_back({offset, length, std::move(done)});
		}, ArchiveMediaAccess::Saver(), [] {}, [] {
			CHECK(QThread::currentThread() == QCoreApplication::instance()->thread());
			return true;
		});
	auto loader = ArchiveMediaLoader(access);
	auto failed = 0;
	auto delivered = 0;
	auto life = loader.parts() | rpl::on_next([&](auto loaded) {
		if (loaded.offset == Media::Streaming::LoadedPart::kFailedOffset) {
			++failed;
		} else {
			++delivered;
			slices.processPart(loaded.offset, std::move(loaded.bytes));
		}
	});
	auto buffer = QByteArray(part, '\0');
	for (auto slice = 0; slice != 4; ++slice) {
		const auto demand = slices.fill(slice * sliceBytes,
			bytes::make_span(buffer.data(), buffer.size()));
		CHECK(demand.state != Reader::FillState::Success);
		auto caller = std::thread([&] {
			loader.resetPriorities();
			for (const auto offset : demand.offsetsFromLoader.values()) {
				loader.load(offset);
			}
		});
		caller.join();
		QCoreApplication::processEvents();
	}
	CHECK(reads.size() == 8 && failed == 0);
	const auto initial = reads;
	for (const auto &read : initial) {
		read.done(QByteArray(read.length, char(read.offset / sliceBytes + 1)));
	}
	QCoreApplication::processEvents();
	CHECK(reads.size() == 16 && failed == 0);
	CHECK(reads[8].offset / sliceBytes == 3);
	for (auto index = size_t(8); index != 16; ++index) {
		const auto read = reads[index];
		read.done(QByteArray(read.length, char(read.offset / sliceBytes + 1)));
	}
	QCoreApplication::processEvents();
	CHECK(slices.fill(3 * sliceBytes, bytes::make_span(buffer.data(), buffer.size())).state == Reader::FillState::Success);
	CHECK(buffer == QByteArray(part, char(4)));
	CHECK(slices._data[0].parts.empty() && slices._data[1].parts.empty());
	for (auto index = size_t(16); index < reads.size(); ++index) {
		const auto read = reads[index];
		read.done(QByteArray(read.length, char(read.offset / sliceBytes + 1)));
		QCoreApplication::processEvents();
	}
	CHECK(delivered == 32 && failed == 0);
	CHECK(slices._data[0].parts.empty() && slices._data[1].parts.empty());
	CHECK(slices._usedSlices.size() == 2);
	slices.fill(0, bytes::make_span(buffer.data(), buffer.size()));
	slices.processPart(0, QByteArray(part, 'a'));
	CHECK(slices.fill(0, bytes::make_span(buffer.data(), buffer.size())).state == Reader::FillState::Success);
	CHECK(buffer == QByteArray(part, 'a'));
}

}

int main(int argc, char **argv) {
	const auto app = QCoreApplication(argc, argv);
	NativeAccess();
	NativeFailures();
	RevisionRevocation();
	EndpointAndLoader();
	ServiceCallbacks();
	ReaderMemory();
	NativeDemandAndThread();
	return Finish();
}

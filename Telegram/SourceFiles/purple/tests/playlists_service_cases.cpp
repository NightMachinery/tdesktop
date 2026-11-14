#include "purple/purple_playlists_service.h"
#include "base/weak_ptr.h"
#include "crl/crl_object_on_thread.h"
#include "rpl/lifetime.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QSemaphore>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>

#include <iostream>

#ifdef Q_OS_MAC
#include <CoreFoundation/CoreFoundation.h>
#endif // Q_OS_MAC

namespace Main {

class Session : public base::has_weak_ptr {
public:
	Account *_account = nullptr;
	rpl::lifetime _lifetime;
	[[nodiscard]] rpl::lifetime &lifetime() { return _lifetime; }

};

class Account {
public:
	Session *session = nullptr;
	[[nodiscard]] Session *maybeSession() const { return session; }
};

} // namespace Main

namespace Purple {

std::optional<AccountRoutingSnapshot> Routing;
AccountKey CurrentAccount = { 0, 101 };

std::optional<AccountRoutingSnapshot> CaptureAccountSyncRouting(
		not_null<Main::Session*> session) {
	return session->_account->maybeSession() == session ? Routing : std::nullopt;
}

AccountKey SessionAccountKey(not_null<Main::Session*>) {
	return CurrentAccount;
}

bool AccountSyncRoutingCurrent(
		not_null<Main::Session*> session,
		const AccountRoutingSnapshot &snapshot) {
	const auto current = CaptureAccountSyncRouting(session);
	return current && current->root == snapshot.root
		&& current->generation == snapshot.generation;
}

bool SameAccountRoutingSnapshot(
		const AccountRoutingSnapshot &a,
		const AccountRoutingSnapshot &b) {
	return a.root == b.root && a.generation == b.generation;
}

} // namespace Purple

#include "playlists-service.inc"

namespace {

using namespace Purple;

int Checks = 0;
int Failures = 0;

void Expect(bool condition, const char *name) {
	++Checks;
	if (!condition) {
		++Failures;
		std::cerr << name << '\n';
	}
}

bool Wait(const std::function<bool()> &done) {
	for (auto i = 0; i != 5000 && !done(); ++i) {
		QCoreApplication::processEvents();
#ifdef Q_OS_MAC
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, true);
#endif // Q_OS_MAC
		QThread::msleep(1);
	}
	Expect(done(), "bounded completion received");
	return done();
}

struct Fixture {
	QTemporaryDir temporary;
	Main::Account account;
	std::unique_ptr<Main::Session> session = std::make_unique<Main::Session>();

	Fixture() {
		Expect(EnsureSyncPrivateDirectory(temporary.path() + u"/accounts"_q)
			== SyncStoreStatus::Ready, "owner directory created");
		auto file = QFile(temporary.path() + u"/accounts/root.toml"_q);
		Expect(file.open(QIODevice::WriteOnly), "owner fixture opens");
		Expect(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner),
			"owner fixture private");
		const auto bytes = SerializeOwnerRecord(CurrentAccount)->toUtf8();
		Expect(file.write(bytes) == bytes.size(), "owner fixture written");
		session->_account = &account;
		account.session = session.get();
		Routing = AccountRoutingSnapshot{
			.root = temporary.path(),
			.owner = CurrentAccount,
			.generation = 1,
			.ready = true,
		};
	}

	~Fixture() {
		InvalidatePlaylistServices();
		session.reset();
		Wait([&] {
			auto locks = QDirIterator(temporary.path(), { u"writer.lock"_q },
				QDir::Files, QDirIterator::Subdirectories);
			return !locks.hasNext();
		});
	}

	PlaylistAccountResult edit(PlaylistAccountEdit request) {
		auto result = std::optional<PlaylistAccountResult>();
		EditAccountPlaylists(session.get(), std::move(request), [&](auto value) {
			Expect(QThread::currentThread() == QCoreApplication::instance()->thread(),
				"edit completion delivered on main thread");
			result = std::move(value);
		});
		Wait([&] { return result.has_value(); });
		return result.value_or(PlaylistAccountResult());
	}
};

PlaylistAccountEdit Create(const QString &title, char seed = 'a') {
	return {
		.kind = PlaylistAccountEditKind::Create,
		.title = title,
		.entropy = QByteArray(10, seed),
		.nowMs = 100,
	};
}

void Exercise() {
	{
		Fixture fixture;
		auto first = fixture.edit(Create(u"First"_q));
		Expect(bool(first) && first.write && first.write->libraryCommitted
			&& first.write->stateCommitted, "first edit durably stores both files");
		if (!first || !first.edit) return;
		const auto id = first.edit->playlistId;
		Expect(first.snapshot.state->unsyncedSince == 100, "edit marks dirty age");
		auto renamed = fixture.edit({
			.kind = PlaylistAccountEditKind::Rename,
			.playlistId = id,
			.title = u"Renamed"_q,
			.nowMs = 200,
		});
		Expect(bool(renamed), "rename applies to fresh stored library");
		Expect(renamed.snapshot.state->unsyncedSince == 100,
			"later edits preserve earliest dirty age");
		const auto undone = fixture.edit({
			.kind = PlaylistAccountEditKind::Undo,
			.undo = renamed.edit->undo,
			.nowMs = 300,
		});
		Expect(bool(undone) && undone.snapshot.library->playlists.at(id).title->value == u"First"_q,
			"undo uses stored register conflict checks");
		const auto invalid = fixture.edit({
			.kind = PlaylistAccountEditKind::Rename,
			.playlistId = id,
			.title = u""_q,
			.nowMs = 400,
		});
		Expect(!invalid && invalid.edit && !*invalid.edit && !invalid.write,
			"invalid core edit cannot write");
		const auto noChange = fixture.edit({
			.kind = PlaylistAccountEditKind::Rename,
			.playlistId = id,
			.title = u"First"_q,
			.nowMs = 400,
		});
		Expect(bool(noChange) && noChange.edit && !noChange.edit->changed && !noChange.write,
			"unchanged edit does not rewrite");
		const auto badTime = fixture.edit(Create(u"Bad time"_q));
		Expect(!badTime && !badTime.write, "id collision refuses write");
		const auto badStamp = fixture.edit({ .title = u"Bad"_q, .nowMs = 0 });
		Expect(!badStamp && badStamp.edit->error == PlaylistEditError::InvalidStamp,
			"invalid dirty stamp refused before edit");
		const auto songs = std::vector<PlaylistAddSongInput>{
			{ .documentId = 501, .peer = { PeerTokenKind::Self, 0 }, .msg = 1 },
			{ .documentId = 502, .peer = { PeerTokenKind::Self, 0 }, .msg = 2 },
		};
		const auto added = fixture.edit({
			.kind = PlaylistAccountEditKind::AddSongs,
			.playlistId = id,
			.songs = songs,
			.nowMs = 500,
		});
		Expect(bool(added) && added.edit->addedCount == 2, "typed add uses core qualification data");
		const auto reordered = fixture.edit({
			.kind = PlaylistAccountEditKind::ReorderSongs,
			.playlistId = id,
			.documentIds = { 502, 501 },
			.nowMs = 600,
		});
		Expect(bool(reordered)
			&& PlaylistVisibleSongIds(reordered.snapshot.library->playlists.at(id))
				== std::vector<uint64_t>({ 502, 501 }), "typed reorder changes display order");
		const auto removed = fixture.edit({
			.kind = PlaylistAccountEditKind::RemoveSongs,
			.playlistId = id,
			.documentIds = { 501 },
			.nowMs = 700,
		});
		Expect(bool(removed) && removed.edit->removedCount == 1, "typed remove retains tombstones");
		const auto copied = fixture.edit({
			.kind = PlaylistAccountEditKind::Copy,
			.entropy = QByteArray(10, 'z'),
			.source = removed.snapshot.library->playlists.at(id),
			.nowMs = 800,
		});
		Expect(bool(copied) && copied.snapshot.library->playlists.size() == 2,
			"typed copy creates distinct playlist");
		const auto deleted = fixture.edit({
			.kind = PlaylistAccountEditKind::Delete,
			.playlistId = id,
			.nowMs = 900,
		});
		Expect(bool(deleted) && deleted.snapshot.library->playlists.at(id).deleted->value,
			"typed delete sets deleted register");
		const auto restored = fixture.edit({
			.kind = PlaylistAccountEditKind::Restore,
			.playlistId = id,
			.nowMs = 1000,
		});
		Expect(bool(restored) && !restored.snapshot.library->playlists.at(id).deleted->value,
			"typed restore clears deleted register");
		Expect(bool(fixture.edit({ .kind = PlaylistAccountEditKind::Delete,
			.playlistId = id, .nowMs = 1100 })), "delete before permanent removal");
		const auto purged = fixture.edit({ .kind = PlaylistAccountEditKind::DeleteForever,
			.playlistId = id, .nowMs = 1200 });
		Expect(bool(purged) && purged.snapshot.library->purged.contains(id)
			&& !purged.snapshot.library->playlists.contains(id), "typed purge uses remove-wins core");
	}
	{
		Fixture fixture;
		auto completions = 0;
		for (auto i = 0; i != 4; ++i) {
			EditAccountPlaylists(fixture.session.get(), Create(QString::number(i), char('a' + i)),
				[&](auto result) { Expect(bool(result), "queued edit succeeds"); ++completions; });
		}
		Wait([&] { return completions == 4; });
		auto read = std::optional<PlaylistAccountResult>();
		ReadAccountPlaylists(fixture.session.get(), [&](auto value) { read = std::move(value); });
		Wait([&] { return read.has_value(); });
		Expect(read && *read && read->snapshot.library->playlists.size() == 4,
			"serialized operations never overwrite preceding edits");
	}
	for (const auto logout : { false, true }) {
		Fixture fixture;
		const auto service = For(fixture.session.get());
		auto entered = QSemaphore();
		auto release = QSemaphore();
		service->worker.with([&](PlaylistAccountWorker&) { entered.release(); release.acquire(); });
		Expect(entered.tryAcquire(1, 5000), "worker barrier entered");
		auto result = std::optional<PlaylistAccountResult>();
		EditAccountPlaylists(fixture.session.get(), Create(u"Retired"_q),
			[&](auto value) { result = std::move(value); });
		if (logout) StopAccountPlaylists(fixture.account);
		else InvalidatePlaylistServices();
		release.release();
		Wait([&] { return result.has_value(); });
		Expect(result && !*result && result->snapshot.status == PlaylistStoreStatus::Changed,
			"revoked queued edit reports changed");
		Expect(!QFile::exists(fixture.temporary.path() + u"/playlists/library.json"_q),
			"revoked edit creates no library");
	}
	{
		Fixture fixture;
		const auto service = For(fixture.session.get());
		auto entered = QSemaphore();
		auto release = QSemaphore();
		service->worker.with([&](PlaylistAccountWorker&) { entered.release(); release.acquire(); });
		Expect(entered.tryAcquire(1, 5000), "destroy barrier entered");
		auto called = false;
		ReadAccountPlaylists(fixture.session.get(), [&](auto) { called = true; });
		auto drained = QSemaphore();
		service->worker.with([&](PlaylistAccountWorker&) { drained.release(); });
		fixture.session.reset();
		fixture.account.session = nullptr;
		release.release();
		Expect(drained.tryAcquire(1, 5000), "destroyed-session queue drained");
		auto mainDrained = false;
		crl::on_main([&] { mainDrained = true; });
		Wait([&] { return mainDrained; });
		Expect(!called, "destroyed session receives no late callback");
	}
	{
		Fixture fixture;
		const auto service = For(fixture.session.get());
		auto drained = QSemaphore();
		auto result = std::optional<PlaylistAccountResult>();
		ReadAccountPlaylists(fixture.session.get(), [&](auto value) { result = std::move(value); });
		service->worker.with([&](PlaylistAccountWorker&) { drained.release(); });
		Expect(drained.tryAcquire(1, 5000), "worker completed before route retirement");
		InvalidatePlaylistServices();
		Wait([&] { return result.has_value(); });
		Expect(result && !*result && result->snapshot.status == PlaylistStoreStatus::Changed,
			"already queued main callback rechecks retired authority");
	}
	{
		Fixture fixture;
		CurrentAccount = { 0, 202 };
		const auto secondary = fixture.edit(Create(u"Secondary"_q));
		Expect(bool(secondary), "secondary account uses routed worker");
		Expect(QFile::exists(fixture.temporary.path() + u"/accounts/0-202/playlists/library.json"_q)
			&& !QFile::exists(fixture.temporary.path() + u"/playlists/library.json"_q),
			"secondary edit cannot write root owner library");
		CurrentAccount = { 0, 101 };
	}
	{
		Fixture fixture;
		Routing.reset();
		const auto held = fixture.edit(Create(u"Unrouted"_q));
		Expect(!held && !held.edit && !held.write, "unready account creates no worker edit");
	}
}

} // namespace

int main(int argc, char *argv[]) {
	QCoreApplication application(argc, argv);
	Exercise();
	InvalidatePlaylistServices();
	std::cout << "PLAYLIST_SERVICE_CHECKS=" << Checks << " FAILURES=" << Failures << '\n';
	return Failures ? 1 : 0;
}

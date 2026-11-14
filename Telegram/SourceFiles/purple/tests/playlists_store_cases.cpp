#include "purple/purple_playlists_store.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>

#include <iostream>
#include <thread>

namespace {

using namespace Purple;

int Checks = 0;
int Failures = 0;

void Expect(bool value, const char *name) {
	++Checks;
	if (!value) {
		++Failures;
		std::cerr << name << '\n';
	}
}

QByteArray Bytes(const QString &path) {
	auto file = QFile(path);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void Put(const QString &path, const QByteArray &bytes) {
	auto file = QFile(path);
	Expect(file.open(QIODevice::WriteOnly), "fixture file opens");
	Expect(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner), "fixture file private");
	Expect(file.write(bytes) == bytes.size(), "fixture bytes written");
}

PlaylistLibrary Library(const QString &title, uint64_t stamp = 100) {
	auto playlist = Playlist();
	playlist.title = PlaylistTitleRegister{ title, stamp };
	playlist.deleted = PlaylistDeletedRegister{ false, stamp };
	playlist.order = PlaylistOrderRegister{ {}, stamp };
	playlist.songs = std::map<uint64_t, PlaylistSongRegister>();
	auto library = PlaylistLibrary();
	library.playlists.emplace(u"pl-aaaaaaaaaaaaaaaa"_q, playlist);
	return library;
}

struct Fixture {
	QTemporaryDir temporary;
	AccountKey active = { 0, 101 };
	uint64_t generation = 1;

	Fixture() {
		Expect(EnsureSyncPrivateDirectory(temporary.path() + u"/accounts"_q) == SyncStoreStatus::Ready,
			"fixture owner directory created");
		Put(temporary.path() + u"/accounts/root.toml"_q,
			SerializeOwnerRecord(AccountKey{ 0, 101 })->toUtf8());
	}

	PlaylistStoreRoute route(AccountKey account = { 0, 101 }) const {
		auto result = PlaylistStoreRoute();
		result.routing.root = temporary.path();
		result.routing.owner = AccountKey{ 0, 101 };
		result.routing.ready = true;
		result.routing.generation = generation;
		result.account = account;
		return result;
	}

	PlaylistStoreRouteCurrent current() {
		return [=](const PlaylistStoreRoute &route) {
			return route.account == active && route.routing.generation == generation;
		};
	}
};

void Exercise() {
	{
		Fixture fixture;
		DesktopPlaylistStore store(fixture.route(), fixture.current());
		auto read = store.open();
		Expect(bool(read), "fresh owner store opens");
		if (!read) return;
		Expect(read.libraryFile.absent() && read.stateFile.absent(), "missing files are distinct from corrupt");
		Expect(read.state->sync, "missing local state defaults sync on");
		Expect(!QFile::exists(store.directory() + u"/library.json"_q), "open does not create a library");
		DesktopPlaylistStore other(fixture.route(), fixture.current());
		Expect(other.open().status == PlaylistStoreStatus::Busy, "one writer per account");
		auto local = *read.state;
		local.sync = false;
		local.preserved.insert(u"future"_q, u"kept"_q);
		const auto first = store.write(read, Library(u"first"_q), local, PlaylistStoreWriteKind::LocalEdit);
		Expect(bool(first) && first.libraryCommitted && first.stateCommitted, "first write confirms both files");
		read = store.read();
		Expect(bool(read) && !read.state->sync, "sync off survives restart read");
		Expect(read.state->preserved.value(u"future"_q).toString() == u"kept"_q, "unknown local fields survive");
		const auto libraryPath = store.directory() + u"/library.json"_q;
		const auto oldLibrary = read.libraryFile.bytes;
		const auto oldIdentity = read.libraryFile.identity;
		const auto quiet = store.write(read, *read.library, *read.state, PlaylistStoreWriteKind::Merge);
		Expect(bool(quiet) && !quiet.libraryMayHaveChanged && !quiet.stateMayHaveChanged, "steady state writes nothing");
		read = store.read();
		Expect(read.libraryFile.identity == oldIdentity, "no-op preserves file identity");
		const auto merged = JoinPlaylistLibraries(*read.library, Library(u"merged"_q, 200), 101);
		Expect(bool(merged), "real core join succeeds");
		const auto saved = store.write(read, merged.library, *read.state, PlaylistStoreWriteKind::Merge);
		Expect(bool(saved) && saved.libraryCommitted, "merge committed");
		Expect(Bytes(store.directory() + u"/library.json.bak"_q) == oldLibrary, "backup holds exact prior bytes");
		Expect(store.write(read, merged.library, local, PlaylistStoreWriteKind::Merge).status == PlaylistStoreStatus::Changed,
			"stale operation cannot interleave after a write");
		read = store.read();
		local = *read.state;
		local.chat = PeerToken{};
		local.memo.emplace(1, PlaylistLocalMemoEntry{ .documentId = 2,
			.state = PlaylistLocalMemoState::Invalid, .classifier = 1 });
		const auto stateBefore = read.stateFile.bytes;
		const auto failed = store.write(read, Library(u"after-library"_q, 300), local,
			PlaylistStoreWriteKind::LocalEdit, [&](SyncDurablePoint point) {
				if (point == SyncDurablePoint::BeforeWrite) {
					const auto current = ParsePlaylistLibrary(Bytes(libraryPath), 101);
					return !current || current.library != Library(u"after-library"_q, 300);
				}
				return true;
			});
		Expect(failed.status == PlaylistStoreStatus::StateWriteFailed && failed.libraryCommitted,
			"state failure reports confirmed library checkpoint");
		Expect(Bytes(store.directory() + u"/local.json"_q) == stateBefore, "memo cannot advance before its library");
		read = store.read();
		Expect(bool(read) && read.library == Library(u"after-library"_q, 300) && read.state->memo.empty(),
			"partial commit recovers library with old memo");
		const auto stateSaved = store.write(read, *read.library, local, PlaylistStoreWriteKind::Merge);
		Expect(bool(stateSaved) && !stateSaved.libraryCommitted && stateSaved.stateCommitted, "retry only saves outstanding state");
		read = store.read();
		++fixture.generation;
		Expect(store.write(read, Library(u"revoked"_q), local, PlaylistStoreWriteKind::Merge).status == PlaylistStoreStatus::Changed,
			"routing generation revokes queued write");
		Expect(Bytes(libraryPath) == read.libraryFile.bytes, "revoked write changes no primary bytes");
	}
	for (const auto point : { SyncDurablePoint::BeforeWrite, SyncDurablePoint::PartialWrite,
		SyncDurablePoint::BeforeRename, SyncDurablePoint::AfterRename,
		SyncDurablePoint::FileBarrier, SyncDurablePoint::DirectoryBarrier }) {
		Fixture fixture;
		DesktopPlaylistStore store(fixture.route(), fixture.current());
		auto read = store.open();
		Expect(bool(read), "fault store opens");
		if (!read) continue;
		Expect(bool(store.write(read, Library(u"old"_q), *read.state, PlaylistStoreWriteKind::LocalEdit)), "seed durable library");
		read = store.read();
		const auto old = read.libraryFile.bytes;
		const auto stateBefore = read.stateFile.bytes;
		const auto result = store.write(read, Library(u"new"_q, 200), *read.state,
			PlaylistStoreWriteKind::LocalEdit, [=](SyncDurablePoint current) { return current != point; });
		Expect(result.status == PlaylistStoreStatus::LibraryWriteFailed && result.libraryMayHaveChanged,
			"injected durability failure is explicit and uncertain");
		Expect(Bytes(store.directory() + u"/local.json"_q) == stateBefore, "unconfirmed library never advances state");
		if (point == SyncDurablePoint::BeforeWrite || point == SyncDurablePoint::PartialWrite
			|| point == SyncDurablePoint::BeforeRename) {
			Expect(Bytes(store.directory() + u"/library.json"_q) == old, "pre-rename failure preserves primary");
		}
		Expect(bool(store.read()), "read reconciles uncertain write");
	}
	for (const auto stateFile : { false, true }) {
		for (const auto &bytes : { QByteArray("broken"), QByteArray("{\"format\":2}") }) {
			Fixture fixture;
			DesktopPlaylistStore store(fixture.route(), fixture.current());
			auto read = store.open();
			Expect(bool(read), "corrupt fixture opens initially");
			if (!read) continue;
			const auto file = store.directory() + (stateFile ? u"/local.json"_q : u"/library.json"_q);
			Put(file, bytes);
			const auto invalid = store.read();
			const auto status = stateFile
				? (bytes == "broken" ? PlaylistStoreStatus::InvalidState : PlaylistStoreStatus::NewerState)
				: (bytes == "broken" ? PlaylistStoreStatus::InvalidLibrary : PlaylistStoreStatus::NewerLibrary);
			Expect(invalid.status == status, "invalid and newer formats classified without defaults");
			Expect(!store.write(read, {}, {}, PlaylistStoreWriteKind::Merge), "stale pre-corruption read cannot overwrite");
			Expect(Bytes(file) == bytes, "invalid/newer file byte-identical");
		}
	}
	{
		Fixture fixture;
		DesktopPlaylistStore store(fixture.route(), fixture.current());
		auto read = store.open();
		Expect(bool(read), "backup failure fixture opens");
		Expect(bool(store.write(read, Library(u"before"_q), {}, PlaylistStoreWriteKind::LocalEdit)), "backup fixture seed");
		read = store.read();
		const auto failed = store.write(read, Library(u"after"_q, 200), {}, PlaylistStoreWriteKind::Merge,
			[](SyncDurablePoint point) { return point != SyncDurablePoint::PartialWrite; });
		Expect(failed.status == PlaylistStoreStatus::BackupFailed, "backup failure prevents merge write");
		Expect(Bytes(store.directory() + u"/library.json"_q) == read.libraryFile.bytes, "backup failure leaves library unchanged");
		Expect(Bytes(store.directory() + u"/local.json"_q) == read.stateFile.bytes, "backup failure leaves memo unchanged");
	}
	{
		Fixture fixture;
		DesktopPlaylistStore store(fixture.route(), fixture.current());
		auto read = store.open();
		Expect(bool(read), "readback failure fixture opens");
		const auto failed = store.write(read, Library(u"checkpoint"_q), {}, PlaylistStoreWriteKind::LocalEdit,
			[&](SyncDurablePoint point) {
				if (point == SyncDurablePoint::AfterRename) {
					Put(store.directory() + u"/library.json"_q, "corrupted-at-readback");
				}
				return true;
			});
		Expect(failed.status == PlaylistStoreStatus::LibraryWriteFailed, "mismatching readback refuses checkpoint");
		Expect(!QFile::exists(store.directory() + u"/local.json"_q), "failed readback leaves memo absent");
		Expect(store.read().status == PlaylistStoreStatus::InvalidLibrary, "corruption is preserved for repair");
	}
	{
		Fixture fixture;
		DesktopPlaylistStore store(fixture.route(), fixture.current());
		const auto read = store.open();
		Expect(bool(read), "reentrant fixture opens");
		auto reentrant = 0;
		const auto result = store.write(read, Library(u"serialized"_q), {}, PlaylistStoreWriteKind::LocalEdit,
			[&](SyncDurablePoint point) {
				if (point == SyncDurablePoint::PartialWrite) {
					Expect(store.read().status == PlaylistStoreStatus::Busy, "read cannot reenter writer");
					Expect(store.write(read, {}, {}, PlaylistStoreWriteKind::Merge).status == PlaylistStoreStatus::Busy,
						"write cannot reenter writer");
					++reentrant;
				}
				return true;
			});
		Expect(bool(result) && reentrant == 2, "library and state writes serialized");
	}
	{
		Fixture fixture;
		DesktopPlaylistStore store(fixture.route(), fixture.current());
		const auto read = store.open();
		Expect(bool(read), "routing-file mutation fixture opens");
		Put(fixture.temporary.path() + u"/local.toml"_q, "changed routing");
		Expect(!store.write(read, Library(u"refused"_q), {}, PlaylistStoreWriteKind::LocalEdit),
			"routing bytes revoke write even if callback remains current");
		Expect(!QFile::exists(store.directory() + u"/library.json"_q), "revoked routing leaves primary absent");
	}
	{
		Fixture fixture;
		Put(fixture.temporary.path() + u"/accounts/root.toml"_q,
			SerializeOwnerRecord(AccountKey{ 0, 202 })->toUtf8());
		DesktopPlaylistStore store(fixture.route(), fixture.current());
		Expect(store.open().status == PlaylistStoreStatus::Changed,
			"changed root owner before open cannot bind stale owner route");
		Expect(!QFile::exists(fixture.temporary.path() + u"/playlists/library.json"_q),
			"owner mismatch creates no primary data");
	}
	{
		Fixture fixture;
		Put(fixture.temporary.path() + u"/accounts/root.toml"_q, "broken");
		DesktopPlaylistStore store(fixture.route(), fixture.current());
		Expect(store.open().status == PlaylistStoreStatus::Changed, "corrupt owner proof refuses store");
	}
	{
		Fixture fixture;
		Expect(QFile::remove(fixture.temporary.path() + u"/accounts/root.toml"_q), "fixture owner proof removed");
		DesktopPlaylistStore store(fixture.route(), fixture.current());
		Expect(store.open().status == PlaylistStoreStatus::Changed, "missing owner proof refuses store");
	}
	{
		Fixture fixture;
		fixture.active = { 0, 202 };
		DesktopPlaylistStore secondary(fixture.route({ 0, 202 }), fixture.current());
		const auto read = secondary.open();
		Expect(bool(read), "secondary account store opens");
		Expect(secondary.directory().endsWith(u"/accounts/0-202/playlists"_q), "secondary account path from shared resolver");
		fixture.active = { 0, 101 };
		Expect(!secondary.write(read, Library(u"other"_q), {}, PlaylistStoreWriteKind::LocalEdit), "account switch revokes write");
	}
	{
		Fixture fixture;
		DesktopPlaylistStore store(fixture.route(), fixture.current());
		const auto read = store.open();
		Expect(bool(read), "symlink fixture opens");
		const auto external = fixture.temporary.path() + u"/outside.json"_q;
		Put(external, "unchanged");
		const auto linked = store.directory() + u"/library.json"_q;
		Expect(QFile::link(external, linked), "fixture symlink created");
		Expect(!store.read(), "symlink file refuses");
		Expect(!store.write(read, {}, {}, PlaylistStoreWriteKind::Merge), "symlink prevents write");
		Expect(Bytes(external) == "unchanged", "symlink target untouched");
	}
	{
		Fixture fixture;
		const auto directory = fixture.temporary.path() + u"/playlists"_q;
		Expect(EnsureSyncPrivateDirectory(directory) == SyncStoreStatus::Ready, "lock fixture directory created");
		const auto external = fixture.temporary.path() + u"/external-lock"_q;
		Put(external, "do-not-read-or-change");
		Expect(QFile::link(external, directory + u"/writer.lock"_q), "lock symlink created");
		DesktopPlaylistStore store(fixture.route(), fixture.current());
		Expect(store.open().status == PlaylistStoreStatus::Changed, "lock symlink refuses before QLockFile");
		Expect(Bytes(external) == "do-not-read-or-change", "lock symlink target untouched");
	}
	{
		Fixture fixture;
		DesktopPlaylistStore store(fixture.route(), fixture.current());
		auto read = store.open();
		Expect(bool(read), "large local file fixture opens");
		if (!read) return;
		auto state = *read.state;
		state.preserved.insert(u"future-large"_q, QString(33 * 1024 * 1024, QChar('x')));
		Expect(bool(store.write(read, {}, state, PlaylistStoreWriteKind::LocalEdit)), "local state above record caps persists");
		Expect(QFileInfo(store.directory() + u"/local.json"_q).size() > 32 * 1024 * 1024,
			"fixture exceeds 32 MiB record expansion cap");
		read = store.read();
		Expect(bool(read) && read.state->preserved.value(u"future-large"_q).toString().size() == 33 * 1024 * 1024,
			"uncapped local file read preserves large future field");
	}
	{
		Fixture fixture;
		DesktopPlaylistStore store(fixture.route(), fixture.current());
		auto read = store.open();
		Expect(bool(read), "empty checkpoint fixture opens");
		auto state = *read.state;
		state.chat = PeerToken{};
		state.memo.emplace(1, PlaylistLocalMemoEntry{ .documentId = 2,
			.state = PlaylistLocalMemoState::Invalid, .classifier = 1 });
		Expect(bool(store.write(read, {}, state, PlaylistStoreWriteKind::Merge)), "empty library checkpoint before first memo");
		Expect(ParsePlaylistLibrary(Bytes(store.directory() + u"/library.json"_q), 101).status == PlaylistLibraryStatus::Valid,
			"empty library materialized before memo");
	}
}

} // namespace

int main(int argc, char **argv) {
	const auto app = QCoreApplication(argc, argv);
	std::thread worker(Exercise);
	worker.join();
	Fixture fixture;
	DesktopPlaylistStore mainThread(fixture.route(), fixture.current());
	Expect(mainThread.open().status == PlaylistStoreStatus::WrongThread, "durable store refuses main-thread IO");
	std::cout << "PLAYLIST_STORE_CHECKS=" << Checks << " FAILURES=" << Failures << '\n';
	return Failures ? 1 : 0;
}

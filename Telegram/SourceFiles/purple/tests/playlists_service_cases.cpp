#include "purple/purple_playlist_folders.h"
#include "purple/purple_playlists_service.h"
#include "purple/purple_playlist_download_registry.h"
#include "purple/purple_playlist_download_files.h"
#include "base/weak_ptr.h"
#include "crl/crl_object_on_thread.h"
#include "rpl/lifetime.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
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

int PlaybackInvalidations = 0;
std::vector<Main::Session*> PlaybackStops;

void InvalidatePlaylistPlayback() { ++PlaybackInvalidations; }
void StopAccountPlaylistPlayback(not_null<Main::Session*> session) {
	PlaybackStops.push_back(session.get());
}

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
		if (!temporary.isValid()) {
			Expect(false, "private service fixture root exists");
			return;
		}
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
		if (!temporary.isValid()) return;
		InvalidatePlaylistServices();
		session.reset();
		Wait([&] {
			auto locks = QDirIterator(temporary.path(), { u"writer.lock"_q },
				QDir::Files, QDirIterator::Subdirectories);
			return !locks.hasNext();
		});
	}

	PlaylistAccountResult use(const QString &id, uint64_t nowMs) {
		if (!temporary.isValid()) return {};
		auto result = std::optional<PlaylistAccountResult>();
		RecordAccountPlaylistUse(session.get(), { id, nowMs }, [&](auto value) {
			Expect(QThread::currentThread() == QCoreApplication::instance()->thread(),
				"recent-use completion delivered on main thread");
			result = std::move(value);
		});
		Wait([&] { return result.has_value(); });
		return result.value_or(PlaylistAccountResult());
	}

	PlaylistAccountResult download(PlaylistAccountDownloadRequest request) {
		if (!temporary.isValid()) return {};
		auto result = std::optional<PlaylistAccountResult>();
		RequestAccountPlaylistDownloads(session.get(), std::move(request), [&](auto value) {
			result = std::move(value);
		});
		Wait([&] { return result.has_value(); });
		return result.value_or(PlaylistAccountResult());
	}

	PlaylistAccountResult edit(PlaylistAccountEdit request) {
		if (!temporary.isValid()) return {};
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

void ExerciseDownloadPruning() {
#ifndef Q_OS_WIN
	for (auto scenario = 0; scenario != 4; ++scenario) {
		const auto protectedCopy = scenario == 1;
		const auto changedContent = scenario == 2;
		const auto alreadyMissing = scenario == 3;
		Fixture fixture;
		const auto created = fixture.edit(Create(u"Retention"_q));
		if (!created || !created.edit) return;
		const auto id = created.edit->playlistId;
		const auto added = fixture.edit({
			.kind = PlaylistAccountEditKind::AddSongs,
			.playlistId = id,
			.songs = { { .documentId = 501, .peer = { PeerTokenKind::Self, 0 }, .msg = 1 } },
			.nowMs = 200,
		});
		if (!added) return;
		auto otherId = QString();
		if (protectedCopy) {
			const auto copied = fixture.edit({
				.kind = PlaylistAccountEditKind::Copy,
				.entropy = QByteArray(10, 'b'),
				.source = added.snapshot.library->playlists.at(id),
				.nowMs = 300,
			});
			if (!copied || !copied.edit) return;
			otherId = copied.edit->playlistId;
			Expect(bool(fixture.edit({ .kind = PlaylistAccountEditKind::Delete,
				.playlistId = otherId, .nowMs = 400 })), "retention fixture enters Recently deleted");
		}
		const auto directory = fixture.temporary.path() + u"/playlists"_q;
		Expect(EnsureSyncPrivateDirectory(directory + u"/files"_q) == SyncStoreStatus::Ready,
			"retention fixture uses a private owned files directory");
		const auto token = QString(32, u'a');
		const auto staged = CreatePlaylistStagingFile(directory, token);
		if (!staged) { Expect(false, "retention staging setup succeeds"); return; }
		const auto bytes = QByteArray("retention-prune-owned-content");
		auto file = QFile(PlaylistStagingFilePath(directory, token));
		Expect(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
			"retention fixture writes complete owned bytes");
		file.close();
		const auto published = PublishPlaylistStagingFile(directory, token,
			{ id, 501, u"mp3"_q, uint64_t(bytes.size()), staged.file.identity,
				QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(), {} });
		if (!published) { Expect(false, "retention owned publication succeeds"); return; }
		const auto authority = std::make_shared<std::atomic<bool>>(true);
		const auto registered = fixture.download({
			.kind = PlaylistDownloadRequestKind::Register,
			.playlistId = id,
			.documentId = 501,
			.expectedSource = *added.snapshot.library->playlists.at(id).songs->at(501).value.source,
			.file = published.file,
			.jobAuthority = authority,
		});
		if (!registered) { Expect(false, "retention fixture registers independently verified bytes"); return; }
		const auto path = PlaylistKeptFilePath(directory, 501, u"mp3"_q);
		const auto changedBytes = QByteArray(bytes.size(), 'x');
		if (changedContent) {
			file.setFileName(path);
			Expect(file.open(QIODevice::WriteOnly)
				&& file.write(changedBytes) == changedBytes.size(), "retention fixture changes actual file digest");
			file.close();
		} else if (alreadyMissing) {
			Expect(QFile::remove(path), "retention fixture removes its own published file");
		}
		const auto removed = fixture.edit({
			.kind = PlaylistAccountEditKind::RemoveSongs,
			.playlistId = id,
			.documentIds = { 501 },
			.nowMs = 500,
		});
		const auto status = changedContent
			? PlaylistDownloadStatus::Changed : PlaylistDownloadStatus::Ready;
		Expect(bool(removed) && removed.edit->removedCount == 1
			&& removed.retentionStatus == status,
			"cleanup refusal never turns a confirmed library edit into a failed edit");
		Expect(QFile::exists(path) == (protectedCopy || changedContent),
			"another unpurged or Recently deleted playlist protects owned file bytes");
		const auto inventory = ReadPlaylistDownloadState(*removed.snapshot.state);
		Expect(bool(inventory) && inventory.state.files.contains(501) == (protectedCopy || changedContent),
			"automatic prune updates only the inventory of successfully removed files");
		if (changedContent) {
			file.setFileName(path);
			Expect(file.open(QIODevice::ReadOnly) && file.readAll() == changedBytes,
				"digest-mismatched file remains byte-exact after refused automatic prune");
			file.close();
			const auto undone = fixture.edit({ .kind = PlaylistAccountEditKind::Undo,
				.undo = removed.edit->undo, .nowMs = 600 });
			Expect(bool(undone) && PlaylistVisibleSongIds(
				undone.snapshot.library->playlists.at(id)) == std::vector<uint64_t>({501}),
				"undo remains available after confirmed remove with deferred retention cleanup");
		}
		if (protectedCopy) {
			const auto purged = fixture.edit({ .kind = PlaylistAccountEditKind::DeleteForever,
				.playlistId = otherId, .nowMs = 600 });
			Expect(bool(purged) && purged.retentionStatus == PlaylistDownloadStatus::Ready
				&& !QFile::exists(path), "permanent removal of last reference prunes the owned file");
		}
	}
#endif
}

void Exercise() {
	for (const auto member : {
		QJsonValue(QJsonObject{ { u"format"_q, 2 }, { u"future"_q, QJsonObject{ { u"opaque"_q, 9 } } } }),
		QJsonValue(u"malformed-client-download-member"_q),
	}) {
		Fixture fixture;
		const auto created = fixture.edit(Create(u"Future local downloads"_q));
		if (!created || !created.edit) return;
		const auto id = created.edit->playlistId;
		auto local = *created.snapshot.state;
		local.preserved.insert(u"desktop_playlist_downloads"_q, member);
		const auto encoded = SerializePlaylistLocalState(local, 101);
		if (!encoded) return;
		auto file = QFile(fixture.temporary.path() + u"/playlists/local.json"_q);
		Expect(file.open(QIODevice::WriteOnly) && file.write(encoded.json) == encoded.json.size(),
			"future download member fixture writes through real core serialization");
		file.close();
		const auto used = fixture.use(id, 200);
		Expect(bool(used) && used.snapshot.state
			&& used.snapshot.state->preserved.value(u"desktop_playlist_downloads"_q) == member
			&& used.snapshot.libraryFile.bytes == created.snapshot.libraryFile.bytes,
			"future or malformed client download member preserves normal recent use");
		const auto renamed = fixture.edit({ .kind = PlaylistAccountEditKind::Rename,
			.playlistId = id, .title = u"Normal rename remains usable"_q, .nowMs = 201 });
		Expect(bool(renamed) && renamed.snapshot.state
			&& renamed.snapshot.state->preserved.value(u"desktop_playlist_downloads"_q) == member,
			"normal library edit preserves the opaque download value exactly");
		const auto before = renamed.snapshot.stateFile.bytes;
		auto refused = std::optional<PlaylistAccountResult>();
		RequestAccountPlaylistDownloads(fixture.session.get(), {
			.kind = PlaylistDownloadRequestKind::Preference, .playlistId = id,
			.preference = PlaylistKeepDownloaded::On,
		}, [&](auto result) { refused = std::move(result); });
		Wait([&] { return refused.has_value(); });
		Expect(refused && !*refused && refused->downloads && !*refused->downloads
			&& refused->snapshot.stateFile.bytes == before
			&& refused->snapshot.libraryFile.bytes == renamed.snapshot.libraryFile.bytes,
			"typed download mutation still refuses opaque future state without writes");
	}

	{
		Fixture fixture;
		const auto created = fixture.edit(Create(u"Retired use"_q));
		if (!created || !created.edit) return;
		const auto service = For(fixture.session.get());
		auto entered = QSemaphore();
		auto release = QSemaphore();
		service->worker.with([&](PlaylistAccountWorker&) { entered.release(); release.acquire(); });
		Expect(entered.tryAcquire(1, 5000), "recent-use retirement barrier entered");
		auto result = std::optional<PlaylistAccountResult>();
		RecordAccountPlaylistUse(fixture.session.get(), { created.edit->playlistId, 200 },
			[&](auto value) { result = std::move(value); });
		StopAccountPlaylists(fixture.account);
		release.release();
		Wait([&] { return result.has_value(); });
		Expect(result && !*result && result->snapshot.status == PlaylistStoreStatus::Changed,
			"retired queued recent use cannot mutate storage or report success");
		auto file = QFile(fixture.temporary.path() + u"/playlists/local.json"_q);
		Expect(file.open(QIODevice::ReadOnly) && file.readAll() == created.snapshot.stateFile.bytes,
			"retired recent use preserves actual state bytes");
	}

	{
		Fixture fixture;
		const auto created = fixture.edit(Create(u"Recent"_q));
		if (!created || !created.edit) return;
		const auto id = created.edit->playlistId;
		const auto path = fixture.temporary.path() + u"/playlists/library.json"_q;
		const auto statePath = fixture.temporary.path() + u"/playlists/local.json"_q;
		auto state = *created.snapshot.state;
		state.sync = false;
		state.preserved.insert(u"future"_q, u"kept"_q);
		const auto encodedState = SerializePlaylistLocalState(state, 101);
		if (!encodedState) return;
		auto stateFile = QFile(statePath);
		Expect(stateFile.open(QIODevice::WriteOnly), "recent-use fixture state opens");
		Expect(stateFile.write(encodedState.json) == encodedState.json.size(), "recent-use fixture state saved");
		stateFile.close();
		const auto original = QByteArray(" \n") + created.snapshot.libraryFile.bytes + "\n";
		auto file = QFile(path);
		Expect(file.open(QIODevice::WriteOnly), "recent-use noncanonical library opens");
		Expect(file.write(original) == original.size(), "recent-use noncanonical library saved");
		file.close();
		const auto identity = SyncPathIdentity(path);
		const auto used = fixture.use(id, 200);
		state.recentUse[id] = 200;
		Expect(bool(used) && used.snapshot.state && *used.snapshot.state == state,
			"recent use changes only recentUse, preserving dirty age, sync and future fields");
		Expect(used.snapshot.libraryFile.bytes == original && SyncPathIdentity(path) == identity
			&& used.write && !used.write->libraryMayHaveChanged,
			"service recent use preserves exact noncanonical library bytes and identity");
		const auto older = fixture.use(id, 150);
		Expect(bool(older) && !older.write && older.snapshot.state->recentUse.at(id) == 200,
			"late use cannot move recent time backward or rewrite state");
		Expect(!fixture.use(u"pl-bbbbbbbbbbbbbbbb"_q, 300), "missing playlist cannot gain recent use");
		Expect(!fixture.use(id, 0), "zero recent-use time is rejected");
		const auto removed = fixture.edit({ .kind = PlaylistAccountEditKind::Delete, .playlistId = id, .nowMs = 400 });
		Expect(bool(removed) && !fixture.use(id, 500), "deleted playlist cannot gain recent use");
	}

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

void ExerciseFolders() {
	Fixture fixture;
	auto request = PlaylistAccountEdit{
		.kind = PlaylistAccountEditKind::CreateHyper,
		.entropy = QByteArray(kPlaylistIdEntropyBytes, 'h'),
		.songs = {
			{ .documentId = 18, .peer = {PeerTokenKind::Self, 0}, .msg = 5 },
			{ .documentId = 11, .peer = {PeerTokenKind::Self, 0}, .msg = 8 },
		},
		.nowMs = 100,
	};
	const auto initial = fixture.edit(request);
	Expect(initial && initial.edit && initial.edit->changed && initial.write,
		"Hyper creation completes through the actual account worker");
	if (!initial || !initial.edit) return;
	const auto id = initial.edit->playlistId;
	const auto placement = ReadPlaylistFolders(*initial.snapshot.state);
	Expect(placement && placement->placements.at(id) == HyperFolderId()
		&& PlaylistVisibleSongIds(initial.snapshot.library->playlists.at(id)) == std::vector<uint64_t>{18, 11},
		"worker reads back the ordered playlist and Hyper placement together");

	InvalidatePlaylistServices();
	request.entropy = QByteArray(kPlaylistIdEntropyBytes, 'i');
	request.nowMs = 200;
	const auto duplicate = fixture.edit(request);
	Expect(duplicate && duplicate.edit && duplicate.edit->playlistId == id
		&& !duplicate.edit->changed && duplicate.snapshot.library->playlists.size() == 1,
		"restart of the account worker reuses the ordered playlist");

	const auto folder = *FormatPlaylistId(QByteArray(kPlaylistIdEntropyBytes, 'f'));
	const auto added = fixture.edit({
		.kind = PlaylistAccountEditKind::SetFolderName,
		.title = u"Focus"_q,
		.folderId = folder,
		.nowMs = 300,
	});
	Expect(bool(added) && added.snapshot.library == duplicate.snapshot.library
		&& ReadPlaylistFolders(*added.snapshot.state)->names.at(folder) == u"Focus"_q,
		"folder creation writes only local state");
	const auto createdInFolder = fixture.edit({
		.kind = PlaylistAccountEditKind::Create,
		.title = u"Focused songs"_q,
		.folderId = folder,
		.entropy = QByteArray(kPlaylistIdEntropyBytes, 'k'),
		.nowMs = 350,
	});
	Expect(createdInFolder && createdInFolder.edit
		&& ReadPlaylistFolders(*createdInFolder.snapshot.state)->placements.at(createdInFolder.edit->playlistId) == folder,
		"ordinary playlist creation and placement commit together");
	const auto moved = fixture.edit({
		.kind = PlaylistAccountEditKind::MoveToFolder,
		.playlistId = id,
		.folderId = folder,
		.nowMs = 400,
	});
	Expect(bool(moved) && ReadPlaylistFolders(*moved.snapshot.state)->placements.at(id) == folder,
		"move existing playlist into ordinary folder");
	const auto removed = fixture.edit({
		.kind = PlaylistAccountEditKind::RemoveFolder,
		.folderId = folder,
		.nowMs = 500,
	});
	Expect(bool(removed) && !ReadPlaylistFolders(*removed.snapshot.state)->placements.contains(id)
		&& removed.snapshot.library->playlists.contains(id), "remove folder with its playlist safely retained");

	request.songs.back() = request.songs.front();
	request.nowMs = 600;
	const auto invalid = fixture.edit(request);
	Expect(!invalid && !invalid.write && invalid.snapshot.library == removed.snapshot.library
		&& invalid.snapshot.state == removed.snapshot.state,
		"rejected song snapshot leaves library and folder state untouched");
}

} // namespace

int main(int argc, char *argv[]) {
	QCoreApplication application(argc, argv);
	ExerciseFolders();
	Exercise();
	ExerciseDownloadPruning();
	{
		Fixture fixture;
		const auto invalidations = PlaybackInvalidations;
		InvalidatePlaylistServices();
		Expect(PlaybackInvalidations == invalidations + 1,
			"service routing retirement invalidates the player port");
		const auto stops = PlaybackStops.size();
		StopAccountPlaylists(fixture.account);
		Expect(PlaybackStops.size() == stops + 1
			&& PlaybackStops.back() == fixture.session.get(),
			"account retirement stops only the captured player session");
	}
	InvalidatePlaylistServices();
	std::cout << "PLAYLIST_SERVICE_CHECKS=" << Checks << " FAILURES=" << Failures << '\n';
	return Failures ? 1 : 0;
}

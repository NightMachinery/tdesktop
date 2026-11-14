#include "purple/purple_playlists_store.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QLockFile>
#include <QtCore/QScopeGuard>
#include <QtCore/QThread>

#include <limits>

namespace Purple {
namespace {

SyncPrivateSnapshot Snapshot(const QString &path, bool owned = false) {
	// WHY: an unlimited QFile read requests an impossible allocation; size
	// races are refused by the snapshot reader without adding a format cap.
	const auto size = std::max(qint64(1), QFileInfo(path).size());
	if (size >= std::numeric_limits<qsizetype>::max()) {
		return {};
	}
	return owned
		? CaptureSyncOwnedFile(path, qsizetype(size))
		: CaptureSyncPrivateFile(path, qsizetype(size));
}

bool SameFile(
		const SyncPrivateSnapshot &a,
		const SyncPrivateSnapshot &b) {
	return a.status == SyncStoreStatus::Ready
		&& b.status == SyncStoreStatus::Ready
		&& a.identity == b.identity
		&& a.parentIdentity == b.parentIdentity
		&& a.bytes == b.bytes;
}

} // namespace

DesktopPlaylistStore::DesktopPlaylistStore(
		PlaylistStoreRoute route,
		PlaylistStoreRouteCurrent current)
: _route(std::move(route))
, _current(std::move(current)) {
}

DesktopPlaylistStore::~DesktopPlaylistStore() = default;

const QString &DesktopPlaylistStore::directory() const {
	return _directory;
}

QString DesktopPlaylistStore::path(const QString &name) const {
	return _directory + u'/' + name;
}

bool DesktopPlaylistStore::sameFile(
		const QString &path,
		const SyncPrivateSnapshot &expected) const {
	return SameFile(Snapshot(path), expected);
}

bool DesktopPlaylistStore::current() const {
	if (!_current || !_current(_route)) {
		return false;
	}
	for (const auto &[path, identity] : _directories) {
		if (SyncDirectoryIdentity(path) != identity) {
			return false;
		}
	}
	return SameFile(_local, Snapshot(_route.routing.root + u"/local.toml"_q, true))
		&& SameFile(_owner, Snapshot(
			_route.routing.root + u"/accounts/root.toml"_q, true));
}

PlaylistStoreRead DesktopPlaylistStore::open() {
	if (_thread && _thread != QThread::currentThread()) {
		return { .status = PlaylistStoreStatus::WrongThread };
	} else if (_writing) {
		return { .status = PlaylistStoreStatus::Busy };
	} else if (_lock) {
		return read();
	}
	const auto &routing = _route.routing;
	const auto files = ResolveAccountFiles(
		_route.account, routing.owner, AccountFileLayout::Desktop);
	if (!SyncStoreWorkerThread()) {
		return { .status = PlaylistStoreStatus::WrongThread };
	} else if (!routing.ready || !routing.generation || !_current
		|| !_current(_route) || !files || !QDir::isAbsolutePath(routing.root)
		|| SyncDirectoryIdentity(routing.root).isEmpty()
		|| !ValidatePlaylistLibrary({}, _route.account.userId)) {
		return { .status = PlaylistStoreStatus::Changed };
	}
	_thread = QThread::currentThread();
	_directory = QDir(routing.root).filePath(files->playlists);
	_directories.emplace(routing.root, SyncDirectoryIdentity(routing.root));
	_local = Snapshot(routing.root + u"/local.toml"_q, true);
	const auto localMatches = _local.status == SyncStoreStatus::Ready
		&& ((_local.absent() && routing.local.state == SettingsFileState::Missing)
			|| (!_local.absent() && routing.local.state == SettingsFileState::Readable
				&& _local.bytes == routing.local.bytes));
	if (!localMatches) {
		return { .status = PlaylistStoreStatus::Changed };
	}
	auto parent = routing.root;
	for (const auto &part : files->playlists.split(u'/')) {
		if (!_current(_route)) {
			return { .status = PlaylistStoreStatus::Changed };
		}
		for (const auto &[path, identity] : _directories) {
			if (SyncDirectoryIdentity(path) != identity) {
				return { .status = PlaylistStoreStatus::Changed };
			}
		}
		parent += u'/' + part;
		if (parent == _directory
			|| (!QFileInfo::exists(parent) && !QFileInfo(parent).isSymLink())) {
			const auto status = EnsureSyncPrivateDirectory(parent);
			if (status != SyncStoreStatus::Ready) {
				return { .status = PlaylistStoreStatus::IoError, .ioStatus = status };
			}
		}
		const auto identity = SyncDirectoryIdentity(parent);
		if (identity.isEmpty()) {
			return { .status = PlaylistStoreStatus::Changed };
		}
		_directories.emplace(parent, identity);
	}
	const auto accounts = routing.root + u"/accounts"_q;
	if (!QFileInfo::exists(accounts)) {
		const auto status = EnsureSyncPrivateDirectory(accounts);
		if (status != SyncStoreStatus::Ready) {
			return { .status = PlaylistStoreStatus::IoError, .ioStatus = status };
		}
	}
	const auto accountsIdentity = SyncDirectoryIdentity(accounts);
	if (accountsIdentity.isEmpty()) {
		return { .status = PlaylistStoreStatus::Changed };
	}
	_directories.emplace(accounts, accountsIdentity);
	_owner = Snapshot(accounts + u"/root.toml"_q, true);
	const auto owner = ParseOwnerRecord(
		QString::fromUtf8(_owner.bytes), accounts + u"/root.toml"_q);
	if (_owner.status != SyncStoreStatus::Ready || _owner.absent()
		|| !owner.ok() || owner.account != routing.owner || !current()) {
		return { .status = PlaylistStoreStatus::Changed };
	}
	const auto lockPath = path(u"writer.lock"_q);
	if (QFileInfo(lockPath).isSymLink()
		|| (QFileInfo::exists(lockPath) && SyncPathIdentity(lockPath).isEmpty())) {
		return { .status = PlaylistStoreStatus::Changed };
	}
	_lock = std::make_unique<QLockFile>(lockPath);
	_lock->setStaleLockTime(0);
	if (!_lock->tryLock(0)) {
		_lock.reset();
		return { .status = PlaylistStoreStatus::Busy };
	}
	return read();
}

PlaylistStoreRead DesktopPlaylistStore::read() {
	if (_thread != QThread::currentThread()) {
		return { .status = PlaylistStoreStatus::WrongThread };
	} else if (_writing) {
		return { .status = PlaylistStoreStatus::Busy };
	} else if (!_lock || !_lock->isLocked()) {
		return { .status = PlaylistStoreStatus::Unopened };
	} else if (!current() || _revision == std::numeric_limits<uint64_t>::max()) {
		_read.reset();
		return { .status = PlaylistStoreStatus::Changed };
	}
	_read.reset();
	auto result = PlaylistStoreRead();
	result.libraryFile = Snapshot(path(u"library.json"_q));
	result.stateFile = Snapshot(path(u"local.json"_q));
	for (const auto *file : { &result.libraryFile, &result.stateFile }) {
		if (file->status != SyncStoreStatus::Ready) {
			return { .status = PlaylistStoreStatus::IoError, .ioStatus = file->status };
		}
	}
	const auto library = result.libraryFile.absent()
		? PlaylistLibraryParseResult{ .status = PlaylistLibraryStatus::Valid }
		: ParsePlaylistLibrary(result.libraryFile.bytes, _route.account.userId, std::nullopt);
	if (!library) {
		return { .status = (library.status == PlaylistLibraryStatus::NewerFormat)
			? PlaylistStoreStatus::NewerLibrary : PlaylistStoreStatus::InvalidLibrary };
	}
	const auto state = result.stateFile.absent()
		? PlaylistLocalParseResult{ .status = PlaylistLocalStatus::Valid }
		: ParsePlaylistLocalState(result.stateFile.bytes, _route.account.userId);
	if (!state) {
		return { .status = (state.status == PlaylistLocalStatus::NewerFormat)
			? PlaylistStoreStatus::NewerState : PlaylistStoreStatus::InvalidState };
	} else if (!current()
		|| !sameFile(path(u"library.json"_q), result.libraryFile)
		|| !sameFile(path(u"local.json"_q), result.stateFile)) {
		return { .status = PlaylistStoreStatus::Changed };
	}
	result.status = PlaylistStoreStatus::Ready;
	result.library = library.library;
	result.state = state.state;
	result.revision = ++_revision;
	_read = result;
	return result;
}

PlaylistStoreWrite DesktopPlaylistStore::write(
		const PlaylistStoreRead &expected,
		const PlaylistLibrary &library,
		const PlaylistLocalState &state,
		PlaylistStoreWriteKind kind,
		const SyncDurableFault &fault) {
	if (_thread != QThread::currentThread()) {
		return { .status = PlaylistStoreStatus::WrongThread };
	} else if (_writing) {
		return { .status = PlaylistStoreStatus::Busy };
	} else if (!_read || !expected || expected.revision != _read->revision
		|| !SameFile(expected.libraryFile, _read->libraryFile)
		|| !SameFile(expected.stateFile, _read->stateFile)
		|| !current() || !sameFile(path(u"library.json"_q), expected.libraryFile)
		|| !sameFile(path(u"local.json"_q), expected.stateFile)) {
		return { .status = PlaylistStoreStatus::Changed };
	}
	const auto encoded = SerializePlaylistLibrary(library, _route.account.userId);
	const auto local = SerializePlaylistLocalState(state, _route.account.userId);
	if (!encoded || !local
		|| (kind != PlaylistStoreWriteKind::LocalEdit && kind != PlaylistStoreWriteKind::Merge)) {
		return { .status = PlaylistStoreStatus::InvalidWrite };
	}
	_writing = true;
	const auto finish = qScopeGuard([&] {
		_writing = false;
		_read.reset();
	});
	const auto guard = [&](SyncDurablePoint point) {
		return current() && (!fault || fault(point)) && current();
	};
	auto result = PlaylistStoreWrite{ .status = PlaylistStoreStatus::Ready };
	const auto writeLibrary = expected.libraryFile.absent()
		|| expected.libraryFile.bytes != encoded.canonical;
	if (writeLibrary && !expected.libraryFile.absent()
		&& kind == PlaylistStoreWriteKind::Merge) {
		const auto backupPath = path(u"library.json.bak"_q);
		const auto backup = Snapshot(backupPath);
		const auto status = WriteSyncDurableFile(
			backupPath, expected.libraryFile.bytes, backup, guard);
		if (status != SyncStoreStatus::Ready) {
			return { .status = PlaylistStoreStatus::BackupFailed, .ioStatus = status };
		}
	}
	if (writeLibrary) {
		result.libraryMayHaveChanged = true;
		const auto status = WriteSyncDurableFile(
			path(u"library.json"_q), encoded.canonical, expected.libraryFile, guard);
		if (status != SyncStoreStatus::Ready) {
			result.status = PlaylistStoreStatus::LibraryWriteFailed;
			result.ioStatus = status;
			return result;
		}
		result.libraryCommitted = true;
	}
	const auto checkpoint = Snapshot(path(u"library.json"_q));
	if (checkpoint.status != SyncStoreStatus::Ready || checkpoint.bytes != encoded.canonical
		|| !sameFile(path(u"local.json"_q), expected.stateFile) || !current()) {
		result.status = PlaylistStoreStatus::StateWriteFailed;
		result.ioStatus = SyncStoreStatus::Changed;
		return result;
	}
	if (expected.stateFile.absent() || expected.stateFile.bytes != local.json) {
		result.stateMayHaveChanged = true;
		const auto stateGuard = [&](SyncDurablePoint point) {
			return guard(point) && sameFile(path(u"library.json"_q), checkpoint);
		};
		const auto status = WriteSyncDurableFile(
			path(u"local.json"_q), local.json, expected.stateFile, stateGuard);
		if (status != SyncStoreStatus::Ready) {
			result.status = PlaylistStoreStatus::StateWriteFailed;
			result.ioStatus = status;
			return result;
		}
		result.stateCommitted = true;
	}
	return result;
}

} // namespace Purple

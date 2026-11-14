#pragma once

#include "purple/purple_account_config.h"
#include "purple/purple_playlists_local.h"
#include "purple/purple_sync_local_store.h"

class QThread;

namespace Purple {

struct PlaylistStoreRoute {
	AccountRoutingSnapshot routing;
	AccountKey account;
};

using PlaylistStoreRouteCurrent =
	std::function<bool(const PlaylistStoreRoute&)>;

enum class PlaylistStoreStatus {
	Ready,
	Unopened,
	WrongThread,
	Changed,
	Busy,
	IoError,
	InvalidLibrary,
	NewerLibrary,
	InvalidState,
	NewerState,
	InvalidWrite,
	BackupFailed,
	LibraryWriteFailed,
	StateWriteFailed,
};

struct PlaylistStoreRead {
	PlaylistStoreStatus status = PlaylistStoreStatus::Unopened;
	SyncStoreStatus ioStatus = SyncStoreStatus::Ready;
	std::optional<PlaylistLibrary> library;
	std::optional<PlaylistLocalState> state;
	SyncPrivateSnapshot libraryFile;
	SyncPrivateSnapshot stateFile;
	uint64_t revision = 0;

	[[nodiscard]] explicit operator bool() const {
		return status == PlaylistStoreStatus::Ready;
	}
};

enum class PlaylistStoreWriteKind {
	LocalEdit,
	Merge,
};

struct PlaylistStoreWrite {
	PlaylistStoreStatus status = PlaylistStoreStatus::InvalidWrite;
	SyncStoreStatus ioStatus = SyncStoreStatus::Ready;
	bool libraryCommitted = false;
	bool stateCommitted = false;
	bool libraryMayHaveChanged = false;
	bool stateMayHaveChanged = false;

	[[nodiscard]] explicit operator bool() const {
		return status == PlaylistStoreStatus::Ready;
	}
};

class DesktopPlaylistStore final {
public:
	DesktopPlaylistStore(
		PlaylistStoreRoute route,
		PlaylistStoreRouteCurrent current);
	~DesktopPlaylistStore();

	[[nodiscard]] PlaylistStoreRead open();
	[[nodiscard]] PlaylistStoreRead read();
	[[nodiscard]] PlaylistStoreWrite write(
		const PlaylistStoreRead &expected,
		const PlaylistLibrary &library,
		const PlaylistLocalState &state,
		PlaylistStoreWriteKind kind,
		const SyncDurableFault &fault = {});
	[[nodiscard]] const QString &directory() const;

private:
	[[nodiscard]] bool current() const;
	[[nodiscard]] bool sameFile(
		const QString &path,
		const SyncPrivateSnapshot &expected) const;
	[[nodiscard]] QString path(const QString &name) const;

	PlaylistStoreRoute _route;
	PlaylistStoreRouteCurrent _current;
	QString _directory;
	std::map<QString, QByteArray> _directories;
	SyncPrivateSnapshot _local;
	SyncPrivateSnapshot _owner;
	std::unique_ptr<QLockFile> _lock;
	std::optional<PlaylistStoreRead> _read;
	QThread *_thread = nullptr;
	uint64_t _revision = 0;
	bool _writing = false;
};

} // namespace Purple

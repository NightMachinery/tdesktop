#include "purple/purple_playlist_download_files.h"

#include "purple/purple_sync_local_store.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QScopeGuard>
#include <limits>

#ifndef Q_OS_WIN
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Purple {
namespace {

#ifndef Q_OS_WIN
QByteArray Identity(const struct stat &value) {
	return QByteArray::number(qulonglong(value.st_dev)) + ':'
		+ QByteArray::number(qulonglong(value.st_ino));
}

bool Owned(const struct stat &value, bool directory) {
	return value.st_uid == ::geteuid() && !(value.st_mode & 0077)
		&& (directory ? S_ISDIR(value.st_mode)
			: S_ISREG(value.st_mode) && value.st_nlink == 1);
}

int OpenFiles(const QString &directory) {
	const auto identity = SyncDirectoryIdentity(directory);
	if (identity.isEmpty()) return -1;
	const auto root = ::open(QFile::encodeName(directory).constData(),
		O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (root < 0) return -1;
	const auto close = qScopeGuard([&] { ::close(root); });
	struct stat value = {};
	if (::fstat(root, &value) || !Owned(value, true) || Identity(value) != identity) {
		return -1;
	}
	const auto files = ::openat(root, "files",
		O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (files < 0) return -1;
	if (::fstat(files, &value) || !Owned(value, true)
		|| SyncDirectoryIdentity(directory) != identity) {
		::close(files);
		return -1;
	}
	return files;
}

PlaylistKeptFileRead Inspect(
		int files,
		const QByteArray &name,
		const QString &extension,
		uint64_t size,
		const PlaylistFileAuthority &authority = {}) {
	if (authority && !authority()) return { PlaylistDownloadStatus::Changed, {} };
	const auto fd = ::openat(files, name.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) return { PlaylistDownloadStatus::IoError, {} };
	const auto close = qScopeGuard([&] { ::close(fd); });
	struct stat before = {}, after = {};
	if (::fstat(fd, &before) || !Owned(before, false) || before.st_size < 0
		|| uint64_t(before.st_size) != size) return {};
	auto hash = QCryptographicHash(QCryptographicHash::Sha256);
	auto count = uint64_t(0);
	char buffer[65536];
	while (true) {
		if (authority && !authority()) return { PlaylistDownloadStatus::Changed, {} };
		const auto read = ::read(fd, buffer, sizeof(buffer));
		if (read < 0) return { PlaylistDownloadStatus::IoError, {} };
		if (!read) break;
		count += uint64_t(read);
		if (count > size) return {};
		hash.addData(QByteArrayView(buffer, read));
	}
	if (authority && !authority()) return { PlaylistDownloadStatus::Changed, {} };
	if (count != size || ::fstat(fd, &after) || !Owned(after, false)
		|| Identity(after) != Identity(before) || after.st_size != before.st_size
		|| after.st_mtime != before.st_mtime || after.st_ctime != before.st_ctime) return {};
	struct stat linked = {};
	if (::fstatat(files, name.constData(), &linked, AT_SYMLINK_NOFOLLOW)
		|| !Owned(linked, false) || Identity(linked) != Identity(before)) return {};
	return { PlaylistDownloadStatus::Ready,
		{ extension, size, Identity(before), hash.result().toHex(), {} } };
}
int OpenIncoming(int files, bool create) {
	if (create && ::mkdirat(files, ".incoming", 0700) && errno != EEXIST) return -1;
	const auto fd = ::openat(files, ".incoming",
		O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) return -1;
	struct stat value = {};
	if (::fstat(fd, &value) || !Owned(value, true)) {
		::close(fd);
		return -1;
	}
	if (create && ::fsync(files)) {
		::close(fd);
		return -1;
	}
	return fd;
}

#endif

} // namespace

QString PlaylistStagingFilePath(const QString &directory, const QString &token) {
	return PlaylistDownloadTokenSafe(token)
		? directory + u"/files/.incoming/"_q + token + u".part"_q : QString();
}

PlaylistKeptFileRead CreatePlaylistStagingFile(
		const QString &directory,
		const QString &token) {
	if (!PlaylistDownloadTokenSafe(token)) return {};
#ifdef Q_OS_WIN
	return { PlaylistDownloadStatus::Unsupported, {} };
#else
	if (EnsureSyncPrivateDirectory(directory + u"/files"_q) != SyncStoreStatus::Ready) {
		return { PlaylistDownloadStatus::IoError, {} };
	}
	const auto files = OpenFiles(directory);
	if (files < 0) return { PlaylistDownloadStatus::Busy, {} };
	const auto closeFiles = qScopeGuard([&] { ::close(files); });
	const auto incoming = OpenIncoming(files, true);
	if (incoming < 0) return { PlaylistDownloadStatus::Busy, {} };
	const auto closeIncoming = qScopeGuard([&] { ::close(incoming); });
	const auto name = token.toLatin1() + ".part";
	const auto fd = ::openat(incoming, name.constData(),
		O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) return { PlaylistDownloadStatus::Busy, {} };
	const auto close = qScopeGuard([&] { ::close(fd); });
	struct stat value = {};
	if (::fstat(fd, &value) || !Owned(value, false) || ::fsync(fd) || ::fsync(incoming)) {
		return { PlaylistDownloadStatus::IoError, {} };
	}
	return { PlaylistDownloadStatus::Ready, { {}, 0, Identity(value), {}, {} } };
#endif
}

PlaylistKeptFileRead InspectPlaylistStagingFile(
		const QString &directory,
		const QString &token,
		const PlaylistOwnedDownloadIntent &intent,
		const PlaylistFileAuthority &authority) {
	if (authority && !authority()) return { PlaylistDownloadStatus::Changed, {} };
	if (!PlaylistDownloadTokenSafe(token) || intent.identity.isEmpty()) return {};
#ifdef Q_OS_WIN
	return { PlaylistDownloadStatus::Unsupported, {} };
#else
	const auto files = OpenFiles(directory);
	if (files < 0) return { PlaylistDownloadStatus::Busy, {} };
	const auto closeFiles = qScopeGuard([&] { ::close(files); });
	const auto incoming = OpenIncoming(files, false);
	if (incoming < 0) return { PlaylistDownloadStatus::Busy, {} };
	const auto closeIncoming = qScopeGuard([&] { ::close(incoming); });
	auto result = Inspect(incoming, token.toLatin1() + ".part", intent.extension, intent.size, authority);
	if (result && result.file.identity != intent.identity) return {};
	return result;
#endif
}

PlaylistDownloadStatus DiscardPlaylistStagingFile(
		const QString &directory,
		const QString &token,
		const QByteArray &identity) {
	if (!PlaylistDownloadTokenSafe(token) || identity.isEmpty()) return PlaylistDownloadStatus::Invalid;
#ifdef Q_OS_WIN
	return PlaylistDownloadStatus::Unsupported;
#else
	const auto files = OpenFiles(directory);
	if (files < 0) {
		return !SyncDirectoryIdentity(directory).isEmpty()
			&& SyncPathIdentity(directory + u"/files"_q) == "missing"
			? PlaylistDownloadStatus::Ready : PlaylistDownloadStatus::Busy;
	}
	const auto closeFiles = qScopeGuard([&] { ::close(files); });
	const auto incoming = OpenIncoming(files, false);
	if (incoming < 0) {
		return SyncPathIdentity(directory + u"/files/.incoming"_q) == "missing"
			? PlaylistDownloadStatus::Ready : PlaylistDownloadStatus::Busy;
	}
	const auto closeIncoming = qScopeGuard([&] { ::close(incoming); });
	const auto name = token.toLatin1() + ".part";
	struct stat value = {};
	if (::fstatat(incoming, name.constData(), &value, AT_SYMLINK_NOFOLLOW)) {
		return errno == ENOENT ? PlaylistDownloadStatus::Ready : PlaylistDownloadStatus::IoError;
	}
	if (!Owned(value, false) || Identity(value) != identity) return PlaylistDownloadStatus::Changed;
	return !::unlinkat(incoming, name.constData(), 0) && !::fsync(incoming)
		? PlaylistDownloadStatus::Ready : PlaylistDownloadStatus::IoError;
#endif
}

PlaylistKeptFileRead PublishPlaylistStagingFile(
		const QString &directory,
		const QString &token,
		const PlaylistOwnedDownloadIntent &intent,
		const PlaylistFileAuthority &authority) {
	if (authority && !authority()) return { PlaylistDownloadStatus::Changed, {} };
	if (!PlaylistDownloadTokenSafe(token) || intent.identity.isEmpty()
		|| intent.sha256.isEmpty()
		|| PlaylistKeptFilePath(directory, intent.documentId, intent.extension).isEmpty()) return {};
#ifdef Q_OS_WIN
	return { PlaylistDownloadStatus::Unsupported, {} };
#else
	const auto files = OpenFiles(directory);
	if (files < 0) return { PlaylistDownloadStatus::Busy, {} };
	const auto closeFiles = qScopeGuard([&] { ::close(files); });
	const auto incoming = OpenIncoming(files, false);
	if (incoming < 0) return { PlaylistDownloadStatus::Busy, {} };
	const auto closeIncoming = qScopeGuard([&] { ::close(incoming); });
	const auto stageName = token.toLatin1() + ".part";
	const auto finalName = QString::number(intent.documentId).toLatin1() + '.' + intent.extension.toLatin1();
	struct stat stage = {}, final = {};
	const auto hasStage = !::fstatat(incoming, stageName.constData(), &stage, AT_SYMLINK_NOFOLLOW);
	const auto hasFinal = !::fstatat(files, finalName.constData(), &final, AT_SYMLINK_NOFOLLOW);
	if (hasFinal) {
		if (Identity(final) != intent.identity) return { PlaylistDownloadStatus::Busy, {} };
		if (hasStage) {
			if (!S_ISREG(stage.st_mode) || stage.st_uid != ::geteuid() || (stage.st_mode & 0077)
				|| stage.st_nlink != 2 || final.st_nlink != 2
				|| Identity(stage) != intent.identity) return {};
			if (authority && !authority()) return { PlaylistDownloadStatus::Changed, {} };
			if (::unlinkat(incoming, stageName.constData(), 0) || ::fsync(incoming)) {
				return { PlaylistDownloadStatus::IoError, {} };
			}
		}
		auto proof = Inspect(files, finalName, intent.extension, intent.size, authority);
		if (proof && (proof.file.identity != intent.identity || proof.file.sha256 != intent.sha256)) return {};
		return proof;
	}
	const auto complete = Inspect(incoming, stageName, intent.extension, intent.size, authority);
	if (!complete || complete.file.identity != intent.identity
		|| complete.file.sha256 != intent.sha256) return complete ? PlaylistKeptFileRead() : complete;
	const auto fd = ::openat(incoming, stageName.constData(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) return { PlaylistDownloadStatus::IoError, {} };
	const auto synced = !::fsync(fd);
	::close(fd);
	if (!synced) return { PlaylistDownloadStatus::IoError, {} };
	if (authority && !authority()) return { PlaylistDownloadStatus::Changed, {} };
	if (::linkat(incoming, stageName.constData(), files, finalName.constData(), 0)) {
		return { PlaylistDownloadStatus::IoError, {} };
	}
	if (::fsync(files) || ::unlinkat(incoming, stageName.constData(), 0) || ::fsync(incoming)) {
		return { PlaylistDownloadStatus::IoError, {} };
	}
	auto proof = Inspect(files, finalName, intent.extension, intent.size, authority);
	if (proof && (proof.file.identity != intent.identity || proof.file.sha256 != intent.sha256)) return {};
	return proof;
#endif
}

QString PlaylistKeptFilePath(
		const QString &directory,
		uint64_t id,
		const QString &extension) {
	const auto key = FormatPlaylistDocumentId(id);
	return key && PlaylistDownloadExtensionSafe(extension)
		? directory + u"/files/"_q + *key + u'.' + extension : QString();
}

PlaylistKeptFileRead InspectPlaylistKeptFile(
		const QString &directory,
		uint64_t id,
		const QString &extension,
		uint64_t expectedSize,
		const PlaylistFileAuthority &authority) {
	if (authority && !authority()) return { PlaylistDownloadStatus::Changed, {} };
	const auto path = PlaylistKeptFilePath(directory, id, extension);
	if (path.isEmpty() || !expectedSize
		|| expectedSize > uint64_t(std::numeric_limits<qint64>::max())) return {};
#ifdef Q_OS_WIN
	return { PlaylistDownloadStatus::Unsupported, {} };
#else
	const auto files = OpenFiles(directory);
	if (files < 0) return { PlaylistDownloadStatus::Busy, {} };
	const auto close = qScopeGuard([&] { ::close(files); });
	const auto name = QString::number(id).toLatin1() + '.' + extension.toLatin1();
	return Inspect(files, name, extension, expectedSize, authority);
#endif
}

PlaylistDownloadStatus RemovePlaylistKeptFile(
		const QString &directory,
		uint64_t id,
		const PlaylistKeptFile &expected,
		const PlaylistFileAuthority &authority) {
	if (authority && !authority()) return PlaylistDownloadStatus::Changed;
	if (PlaylistKeptFilePath(directory, id, expected.extension).isEmpty()
		|| !expected.size) return PlaylistDownloadStatus::Invalid;
#ifdef Q_OS_WIN
	return PlaylistDownloadStatus::Unsupported;
#else
	const auto files = OpenFiles(directory);
	if (files < 0) return PlaylistDownloadStatus::Busy;
	const auto close = qScopeGuard([&] { ::close(files); });
	const auto name = QString::number(id).toLatin1() + '.' + expected.extension.toLatin1();
	struct stat linked = {};
	if (::fstatat(files, name.constData(), &linked, AT_SYMLINK_NOFOLLOW)) {
		return errno == ENOENT ? PlaylistDownloadStatus::Ready : PlaylistDownloadStatus::IoError;
	}
	const auto read = Inspect(files, name, expected.extension, expected.size, authority);
	if (!read) return read.status;
	if (read.file.identity != expected.identity || read.file.sha256 != expected.sha256) {
		return PlaylistDownloadStatus::Changed;
	}
	if (authority && !authority()) return PlaylistDownloadStatus::Changed;
	if (::unlinkat(files, name.constData(), 0) || ::fsync(files)) {
		return PlaylistDownloadStatus::IoError;
	}
	return PlaylistDownloadStatus::Ready;
#endif
}

} // namespace Purple

#include "purple/purple_archive_files.h"
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <limits>
#include <vector>
#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#include <aclapi.h>
#include <io.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Purple {
namespace {

bool SyncAbsentParent(QString path) {
	for (auto depth = 0; depth != 16; ++depth) {
		const auto parent = QFileInfo(path).absolutePath();
		if (parent == path) {
			return false;
		}
		const auto info = QFileInfo(parent);
		if (info.isSymLink()) {
			return false;
		}
		if (info.exists()) {
			return DesktopArchiveSyncDirectory(parent);
		}
		path = parent;
	}
	return false;
}

#ifdef Q_OS_WIN
bool OwnerAcl(const QString &path, bool apply) {
	auto token = HANDLE();
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		return false;
	}
	auto size = DWORD(0);
	GetTokenInformation(token, TokenUser, nullptr, 0, &size);
	auto storage = QByteArray(int(size), char(0));
	const auto read = GetTokenInformation(token, TokenUser, storage.data(), size, &size);
	CloseHandle(token);
	if (!read) {
		return false;
	}
	const auto user = reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid;
	auto acl = PACL();
	auto descriptor = PSECURITY_DESCRIPTOR();
	auto owner = PSID();
	if (apply) {
		auto access = EXPLICIT_ACCESSW();
		access.grfAccessPermissions = GENERIC_ALL;
		access.grfAccessMode = SET_ACCESS;
		access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
		access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
		access.Trustee.TrusteeType = TRUSTEE_IS_USER;
		access.Trustee.ptstrName = static_cast<LPWSTR>(user);
		if (SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS) {
			return false;
		}
		const auto result = SetNamedSecurityInfoW(
			const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())), SE_FILE_OBJECT,
			DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
			nullptr, acl, nullptr);
		LocalFree(acl);
		if (result != ERROR_SUCCESS) {
			return false;
		}
	}
	if (GetNamedSecurityInfoW(const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())),
			SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
			&owner, nullptr, &acl, nullptr, &descriptor)
			!= ERROR_SUCCESS
		|| !acl) {
		if (descriptor) {
			LocalFree(descriptor);
		}
		return false;
	}
	auto valid = owner && EqualSid(user, owner);
	for (auto i = DWORD(0); i != acl->AceCount; ++i) {
		auto ace = static_cast<void*>(nullptr);
		if (!GetAce(acl, i, &ace)) {
			valid = false;
			break;
		}
		const auto header = static_cast<ACE_HEADER*>(ace);
		if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
			valid = false;
			break;
		}
		const auto allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
		if (!EqualSid(user, &allowed->SidStart)) {
			valid = false;
			break;
		}
	}
	LocalFree(descriptor);
	return valid;
}
#endif

}

QByteArray DesktopArchiveIdentity(const QString &path) {
#ifdef Q_OS_WIN
	const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), 0,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		return {};
	}
	auto info = BY_HANDLE_FILE_INFORMATION();
	const auto ok = GetFileInformationByHandle(handle, &info)
					&& !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
					&& ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
						|| info.nNumberOfLinks == 1);
	CloseHandle(handle);
	return ok ? QByteArray::number(info.dwVolumeSerialNumber) + ':'
					+ QByteArray::number(info.nFileIndexHigh) + ':'
					+ QByteArray::number(info.nFileIndexLow)
			  : QByteArray();
#else
	struct stat info = {};
	if (::lstat(QFile::encodeName(path).constData(), &info) || info.st_uid != ::geteuid()
		|| (!S_ISDIR(info.st_mode) && (!S_ISREG(info.st_mode) || info.st_nlink != 1))) {
		return {};
	}
	return QByteArray::number(qulonglong(info.st_dev)) + ':'
		   + QByteArray::number(qulonglong(info.st_ino));
#endif
}

std::optional<DesktopArchiveSourceIdentity> DesktopArchiveInspectSource(
	const QString &path, uint64_t size) {
	const auto native = DesktopArchiveIdentity(path);
	const auto info = QFileInfo(path);
	if (native.isEmpty() || !info.isFile() || !info.lastModified().isValid()
		|| info.size() < 0 || uint64_t(info.size()) != size) {
		return {};
	}
	return DesktopArchiveSourceIdentity{
		native, info.lastModified().toMSecsSinceEpoch(), size};
}

bool DesktopArchiveSourceMatches(
	const QFile &file, const DesktopArchiveSourceIdentity &identity) {
	if (!file.isOpen() || identity.native.isEmpty()
		|| identity.size > uint64_t(std::numeric_limits<qint64>::max())) {
		return false;
	}
#ifdef Q_OS_WIN
	const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(int(file.handle())));
	auto info = BY_HANDLE_FILE_INFORMATION();
	if (handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle, &info)
		|| (info.dwFileAttributes
			& (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
		|| info.nNumberOfLinks != 1) {
		return false;
	}
	const auto native = QByteArray::number(info.dwVolumeSerialNumber) + ':'
						+ QByteArray::number(info.nFileIndexHigh) + ':'
						+ QByteArray::number(info.nFileIndexLow);
	const auto size = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
	const auto ticks = (uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32)
					   | info.ftLastWriteTime.dwLowDateTime;
	const auto modified = qint64(ticks / 10000) - 11644473600000LL;
#else
	struct stat info = {};
	if (::fstat(int(file.handle()), &info) || !S_ISREG(info.st_mode) || info.st_nlink != 1
		|| info.st_uid != ::geteuid() || info.st_size < 0) {
		return false;
	}
	const auto native = QByteArray::number(qulonglong(info.st_dev)) + ':'
						+ QByteArray::number(qulonglong(info.st_ino));
	const auto size = uint64_t(info.st_size);
#ifdef Q_OS_MAC
	const auto modified
		= qint64(info.st_mtimespec.tv_sec) * 1000 + info.st_mtimespec.tv_nsec / 1000000;
#else
	const auto modified
		= qint64(info.st_mtim.tv_sec) * 1000 + info.st_mtim.tv_nsec / 1000000;
#endif
#endif
	return native == identity.native && size == identity.size
		   && modified == identity.modified;
}

bool DesktopArchiveOpenSource(
	const QString &path, const DesktopArchiveSourceIdentity &identity, QFile &file) {
	if (file.isOpen() || DesktopArchiveIdentity(path) != identity.native) {
		return false;
	}
#ifdef Q_OS_WIN
	file.setFileName(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return false;
	}
#else
	const auto descriptor
		= ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (descriptor < 0) {
		return false;
	}
	if (!file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
		::close(descriptor);
		return false;
	}
#endif
	if (!DesktopArchiveSourceMatches(file, identity)
		|| DesktopArchiveIdentity(path) != identity.native) {
		file.close();
		return false;
	}
	return true;
}

bool DesktopArchivePrivate(const QString &path, bool directory) {
	const auto info = QFileInfo(path);
	if (DesktopArchiveIdentity(path).isEmpty() || info.isSymLink()
		|| (directory ? !info.isDir() : !info.isFile())) {
		return false;
	}
#ifdef Q_OS_WIN
	return OwnerAcl(path, false);
#else
	struct stat native = {};
	return ::lstat(QFile::encodeName(path).constData(), &native) == 0
		   && !(native.st_mode & 0077)
		   && (native.st_mode & (directory ? 0700 : 0600)) == (directory ? 0700 : 0600);
#endif
}

bool DesktopArchiveSyncDirectory(const QString &path) {
#ifdef Q_OS_WIN
	const auto directory = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
	if (DesktopArchiveIdentity(path).isEmpty() || !QFileInfo(path).isDir()) {
		return false;
	}
	auto mount = QString(32768, QChar());
	if (!GetVolumePathNameW(reinterpret_cast<LPCWSTR>(directory.utf16()),
			reinterpret_cast<LPWSTR>(mount.data()), DWORD(mount.size()))) {
		return false;
	}
	auto volume = QString(32768, QChar());
	if (!GetVolumeNameForVolumeMountPointW(reinterpret_cast<LPCWSTR>(mount.utf16()),
			reinterpret_cast<LPWSTR>(volume.data()), DWORD(volume.size()))) {
		return false;
	}
	volume.resize(volume.indexOf(QChar()));
	if (!volume.endsWith(u'\\')) {
		return false;
	}
	volume.chop(1);
	const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(volume.utf16()), GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		return false;
	}
	const auto flushed = FlushFileBuffers(handle) != 0;
	const auto closed = CloseHandle(handle) != 0;
	return flushed && closed;
#else
	const auto descriptor = ::open(QFile::encodeName(path).constData(),
		O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (descriptor < 0) {
		return false;
	}
	const auto ok = ::fsync(descriptor) == 0;
	const auto closed = ::close(descriptor) == 0;
	return ok && closed;
#endif
}

bool DesktopArchiveDirectory(const QString &path) {
	const auto info = QFileInfo(path);
	if (!info.exists() && !info.isSymLink()) {
#ifdef Q_OS_WIN
		if (!QDir().mkdir(path) || !OwnerAcl(path, true)) {
			return false;
		}
#else
		if (::mkdir(QFile::encodeName(path).constData(), 0700) != 0) {
			return false;
		}
#endif
		if (!DesktopArchiveSyncDirectory(info.absolutePath())) {
			return false;
		}
	}
	return DesktopArchivePrivate(path, true) && DesktopArchiveExcludeBackup(path);
}

std::optional<QByteArray> DesktopArchiveRead(const QString &path, qint64 limit) {
	const auto identity = DesktopArchiveIdentity(path);
	if (limit < 0 || limit > 32 * 1024 * 1024 || identity.isEmpty()
		|| !DesktopArchivePrivate(path, false)) {
		return {};
	}
	auto file = QFile(path);
#ifdef Q_OS_WIN
	if (!file.open(QIODevice::ReadOnly) || file.size() > limit) {
		return {};
	}
#else
	const auto descriptor
		= ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (descriptor < 0) {
		return {};
	}
	struct stat opened = {};
	const auto openedIdentity = [&] {
		return QByteArray::number(qulonglong(opened.st_dev)) + ':'
			   + QByteArray::number(qulonglong(opened.st_ino));
	};
	if (::fstat(descriptor, &opened) || !S_ISREG(opened.st_mode) || opened.st_nlink != 1
		|| opened.st_uid != ::geteuid() || (opened.st_mode & 0077) || opened.st_size < 0
		|| opened.st_size > limit || openedIdentity() != identity) {
		::close(descriptor);
		return {};
	}
	if (!file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
		::close(descriptor);
		return {};
	}
#endif
	const auto bytes = file.read(limit + 1);
	return file.error() == QFileDevice::NoError && file.atEnd() && bytes.size() <= limit
				   && bytes.size() == file.size()
				   && DesktopArchiveIdentity(path) == identity
				   && DesktopArchivePrivate(path, false)
			   ? std::optional(bytes)
			   : std::nullopt;
}

bool DesktopArchiveWrite(const QString &path, const QByteArray &bytes) {
	const auto old = QFileInfo(path);
	if ((old.exists() || old.isSymLink()) && !DesktopArchivePrivate(path, false)) {
		return false;
	}
	auto file = QSaveFile(path);
	file.setDirectWriteFallback(false);
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
#ifdef Q_OS_UNIX
	if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
		return false;
	}
#endif
	if (file.write(bytes) != bytes.size() || !file.flush()) {
		return false;
	}
#ifdef Q_OS_WIN
	const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(int(file.handle())));
	if (handle == INVALID_HANDLE_VALUE || !FlushFileBuffers(handle)) {
		return false;
	}
#else
	if (::fsync(int(file.handle())) != 0) {
		return false;
	}
#endif
	if (!file.commit()) {
		return false;
	}
#ifdef Q_OS_WIN
	if (!OwnerAcl(path, true)) {
		return false;
	}
#endif
	const auto read = DesktopArchiveRead(path, bytes.size());
	return read && *read == bytes && DesktopArchiveExcludeBackup(path)
		   && DesktopArchiveSyncDirectory(old.absolutePath());
}

bool DesktopArchiveRemoveFile(const QString &path) {
	const auto info = QFileInfo(path);
	if (!info.exists() && !info.isSymLink()) {
		return SyncAbsentParent(path);
	}
	return DesktopArchivePrivate(path, false) && QFile::remove(path)
		   && DesktopArchiveSyncDirectory(info.absolutePath());
}

bool DesktopArchiveRemoveTreePage(const QString &root, int limit, bool &complete) {
	complete = false;
	if (limit < 1 || limit > 128) {
		return false;
	}
	const auto info = QFileInfo(root);
	if (!info.exists() && !info.isSymLink()) {
		complete = true;
		return SyncAbsentParent(root);
	}
	if (!DesktopArchivePrivate(root, true)) {
		return false;
	}
	auto iterator = QDirIterator(root,
		QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
		QDirIterator::Subdirectories);
	auto visited = 0;
	auto emptyDirectories = std::vector<QString>();
	while (iterator.hasNext() && visited++ < limit) {
		const auto path = iterator.next();
		const auto entry = iterator.fileInfo();
		if (path.mid(root.size()).count('/') > 8) {
			return false;
		}
		if (entry.isSymLink() || DesktopArchiveIdentity(path).isEmpty()) {
			return false;
		}
		if (entry.isDir()) {
			if (!DesktopArchivePrivate(path, true)) {
				return false;
			}
			if (QDir(path).isEmpty() && emptyDirectories.size() < size_t(limit)) {
				emptyDirectories.push_back(path);
			}
		} else {
			if (!DesktopArchiveRemoveFile(path)) {
				return false;
			}
		}
	}
	for (auto i = emptyDirectories.rbegin(); i != emptyDirectories.rend(); ++i) {
		if (!QDir().rmdir(*i)
			|| !DesktopArchiveSyncDirectory(QFileInfo(*i).absolutePath())) {
			return false;
		}
	}
	if (QDir(root).isEmpty()) {
		if (!QDir().rmdir(root) || !DesktopArchiveSyncDirectory(info.absolutePath())) {
			return false;
		}
		complete = true;
	}
	return true;
}

#ifndef Q_OS_MAC
bool DesktopArchiveExcludeBackup(const QString &) { return true; }
#endif

}

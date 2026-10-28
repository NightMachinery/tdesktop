#pragma once

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <functional>
#include <memory>
#include <optional>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace Purple {

struct EmojiExportDirectoryIdentity {
	quint64 device = 0;
	quint64 file = 0;
	friend bool operator==(const EmojiExportDirectoryIdentity &,
		const EmojiExportDirectoryIdentity &) = default;
};

inline std::optional<EmojiExportDirectoryIdentity> EmojiExportDirectoryId(
		const QString &path) {
	if (!QDir::isAbsolutePath(path) || QDir::cleanPath(path) != path) return {};
	auto info = QFileInfo(path);
	while (!info.isRoot()) {
		if (info.isSymLink() || !info.isDir()) return {};
		info = QFileInfo(info.absolutePath());
	}
#ifdef Q_OS_WIN
	const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
		FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
		nullptr);
	if (handle == INVALID_HANDLE_VALUE) return {};
	const auto guard = std::unique_ptr<void, decltype(&CloseHandle)>(handle, &CloseHandle);
	auto details = BY_HANDLE_FILE_INFORMATION();
	const auto ok = GetFileInformationByHandle(handle, &details);
	if (!ok || (details.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
		|| !(details.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return {};
	return EmojiExportDirectoryIdentity{ details.dwVolumeSerialNumber,
		(quint64(details.nFileIndexHigh) << 32) | details.nFileIndexLow };
#else
	struct stat details = {};
	if (::lstat(QFile::encodeName(path).constData(), &details)
		|| !S_ISDIR(details.st_mode)) return {};
	return EmojiExportDirectoryIdentity{ quint64(details.st_dev), quint64(details.st_ino) };
#endif
}

inline bool EmojiExportParentMatches(const QString &path,
		const std::optional<EmojiExportDirectoryIdentity> &identity) {
	return identity && EmojiExportDirectoryId(QFileInfo(path).absolutePath()) == identity;
}

inline bool RemoveEmojiExportFile(const QString &path,
		const std::optional<EmojiExportDirectoryIdentity> &identity,
		const std::function<bool()> &current = {}) {
	if (current && !current()) return false;
	const auto info = QFileInfo(path);
	return EmojiExportParentMatches(path, identity)
		&& ((!info.exists() && !info.isSymLink()) || QFile::remove(path));
}

}

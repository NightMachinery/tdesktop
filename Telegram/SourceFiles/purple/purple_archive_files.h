#pragma once
#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <cstdint>
#include <optional>

class QFile;

namespace Purple {

struct DesktopArchiveSourceIdentity {
	QByteArray native;
	qint64 modified = 0;
	uint64_t size = 0;

};

[[nodiscard]] std::optional<DesktopArchiveSourceIdentity> DesktopArchiveInspectSource(
	const QString &path, uint64_t size);
[[nodiscard]] bool DesktopArchiveOpenSource(
	const QString &path, const DesktopArchiveSourceIdentity &identity, QFile &file);
[[nodiscard]] bool DesktopArchiveSourceMatches(
	const QFile &file, const DesktopArchiveSourceIdentity &identity);

[[nodiscard]] QByteArray DesktopArchiveIdentity(const QString &path);
[[nodiscard]] bool DesktopArchivePrivate(const QString &path, bool directory);
[[nodiscard]] bool DesktopArchiveDirectory(const QString &path);
[[nodiscard]] bool DesktopArchiveSyncDirectory(const QString &path);
[[nodiscard]] bool DesktopArchiveExcludeBackup(const QString &path);
[[nodiscard]] std::optional<QByteArray> DesktopArchiveRead(
	const QString &path, qint64 limit);
[[nodiscard]] bool DesktopArchiveWrite(const QString &path, const QByteArray &bytes);
[[nodiscard]] bool DesktopArchiveRemoveFile(const QString &path);
[[nodiscard]] bool DesktopArchiveRemoveTreePage(
	const QString &root, int limit, bool &complete);

}

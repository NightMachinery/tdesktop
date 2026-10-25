#pragma once
#include "purple/purple_archive_sql.h"
#include <QtCore/QIODevice>

namespace Purple {

struct DesktopArchiveMediaCopy {
	QString hash;
	uint64_t size = 0;
};

[[nodiscard]] bool DesktopArchivePublishMedia(const QString &root,
	const AccountKey &account, const ArchiveSqlSecurity &security, QIODevice &source,
	const DesktopArchiveMediaCopy &copy, const std::function<bool()> &current);
[[nodiscard]] bool DesktopArchiveReadMedia(const QString &root, const AccountKey &account,
	const ArchiveSqlSecurity &security, const DesktopArchiveMediaCopy &copy,
	const std::function<bool(const QByteArray &)> &sink,
	const std::function<bool()> &current);

}

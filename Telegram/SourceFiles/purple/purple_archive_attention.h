#pragma once
#include "purple/purple_archive_lifecycle.h"

namespace Purple {
struct DesktopArchiveAttention {
	bool present = false;
	bool valid = false;
	ArchiveCaptureGap gap;
};
[[nodiscard]] DesktopArchiveAttention ReadDesktopArchiveAttention(const QString &root,
	const ArchiveStoreEpoch &epoch, const ArchiveSqlSecurity &security);
[[nodiscard]] bool WriteDesktopArchiveAttention(const QString &root,
	const ArchiveStoreEpoch &epoch, const ArchiveSqlSecurity &security,
	const QString &reason, int64 time);
}

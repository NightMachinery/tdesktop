#pragma once

#include "purple/purple_archive_queries.h"
#include "purple/purple_archive_sql_maintenance.h"
#include <functional>

namespace Purple {

using DesktopArchiveCommit = std::function<bool(const ArchiveSqlProgram&)>;

struct DesktopArchiveMaintenanceRecord {
	bool valid = false;
	ArchiveState header;
	std::optional<ArchiveSqlPurge> purge;
};

struct DesktopArchivePurgePreview {
	ArchiveQueryStatus status = ArchiveQueryStatus::Unavailable;
	ArchiveStoreEpoch epoch;
	std::optional<ArchivePurgeConfirmation> confirmation;
	std::array<int64, 7> counts = {};
	bool maintenance = false;
};

struct DesktopArchiveMaintenanceStep {
	bool valid = false;
	bool finished = false;
	size_t processedRows = 0;
	ArchivePurgePhase phase = ArchivePurgePhase::Versions;
	std::vector<QString> releaseStagedJobs;
	std::vector<QString> revokeReaders;
};

struct DesktopArchiveGarbageStep {
	bool valid = false;
	bool complete = false;
};

[[nodiscard]] DesktopArchiveMaintenanceRecord
InspectDesktopArchiveMaintenance(DesktopArchiveSqlite &database,
	const ArchiveStoreEpoch &epoch,
	const ArchiveSqlSecurity &security);
[[nodiscard]] DesktopArchivePurgePreview PrepareDesktopArchivePurge(
	DesktopArchiveSqlite &database,
	const ArchiveStoreEpoch &epoch,
	ArchivePurgeKind kind,
	const ArchivePurgeScope &scope,
	const ArchiveSqlSecurity &security);
[[nodiscard]] bool BeginDesktopArchivePurge(DesktopArchiveSqlite &database,
	const ArchiveStoreEpoch &epoch,
	const ArchivePurgeConfirmation &confirmation,
	const QString &operation,
	const ArchiveSqlSecurity &security,
	const DesktopArchiveCommit &commit);
[[nodiscard]] DesktopArchiveMaintenanceStep ContinueDesktopArchivePurge(
	DesktopArchiveSqlite &database,
	const ArchiveStoreEpoch &epoch,
	const ArchiveSqlSecurity &security,
	const DesktopArchiveCommit &commit);
[[nodiscard]] DesktopArchiveGarbageStep ContinueDesktopArchiveGarbage(
	DesktopArchiveSqlite &database,
	const ArchiveStoreEpoch &epoch,
	const ArchiveSqlSecurity &security,
	const QString &root,
	const std::function<bool()> &current,
	QString &cancelledJobCursor,
	bool &cancelledJobsComplete,
	const DesktopArchiveCommit &commit);

}

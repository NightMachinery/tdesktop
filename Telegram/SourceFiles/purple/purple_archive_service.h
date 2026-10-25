#pragma once

#include "purple/purple_archive_files.h"
#include "purple/purple_archive_acquired_source.h"
#include "purple/purple_desktop_archive_store.h"

namespace Main {
class Account;
class Session;
}

namespace Purple {

void StartArchiveServices();
[[nodiscard]] Fn<void()> ArchiveDiscardAction(not_null<Main::Session*> session);
void QueueArchiveRetentionGap(not_null<Main::Session*> session, QString reason);
void QueueArchiveLifecycleFact(not_null<Main::Session*> session, ArchiveEvent event);
[[nodiscard]] Fn<void(uint64_t, QByteArray)> ArchiveStreamSink(not_null<Main::Session*> session,
	DesktopArchiveObservation observation, uint64_t size);
void QueueArchiveObservation(not_null<Main::Session*> session, ArchiveEvent event);
void QueueArchiveAvailableMedia(not_null<Main::Session*> session, ArchiveMessageKey key,
	QString observationId, QString contentHash, QString identity, QByteArray bytes,
	QString path, uint64_t size,
	std::optional<DesktopArchiveSourceIdentity> source = std::nullopt);
void RemoveArchiveAtLogout(Main::Account &account);
[[nodiscard]] QString ArchiveStorageSummary(not_null<Main::Session*> session);
[[nodiscard]] rpl::producer<> ArchiveStorageChanges();
void ReadArchiveVersions(not_null<Main::Session*> session, ArchiveMessageKey key,
	QString before, std::optional<uint64_t> revision,
	Fn<void(DesktopArchiveHistoryPage)> done);

}

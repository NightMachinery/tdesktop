#pragma once

#include "purple/purple_archive_files.h"
#include "purple/purple_markdown_publication.h"
#include "purple/purple_markdown_source.h"
#include "purple/purple_archive_markdown.h"
#include "purple/purple_archive_acquired_source.h"
#include "purple/purple_desktop_archive_store.h"

namespace Main {
class Account;
class Session;
}

namespace Purple {

struct DesktopMarkdownCopyScope {
	ArchiveStoreEpoch epoch;
	std::shared_ptr<DesktopMarkdownPublication> publication;
	std::function<bool()> current;
	uint64_t stamp = 0;
};
struct DesktopMarkdownCopySource {
	bool current = false;
	std::optional<QString> text;
};
[[nodiscard]] std::optional<DesktopMarkdownCopyScope> OpenDesktopMarkdownCopy(
	not_null<Main::Session*> session);
void CopyArchiveVersionAsMarkdown(not_null<Main::Session*> session,
	DesktopArchiveMarkdownRequest request, std::function<bool()> current);
void QueueDesktopResolvedRichBody(not_null<Main::Session*> session, ArchiveEvent event);
void ReadDesktopMarkdownSource(not_null<Main::Session*> session,
	DesktopMarkdownCopyScope scope, DesktopMarkdownBody body,
	Fn<void(DesktopMarkdownCopySource)> done);

struct DesktopComposeIngress {
	ArchiveStoreEpoch epoch;
	std::function<bool()> current;
	Fn<void(ArchiveEvent, ArchiveSource)> submit;
};
[[nodiscard]] std::optional<DesktopComposeIngress> DesktopComposeSourceIngress(
	not_null<Main::Session*> session, ArchiveChatKey chat);

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
	Fn<void(DesktopArchiveHistoryPage)> done, QString history = {});
void CloseArchiveHistory(not_null<Main::Session*> session, QString id, ArchiveStoreEpoch epoch);
void OpenArchiveDeletedView(not_null<Main::Session*> session, ArchiveViewScope scope,
	ArchiveTranscriptOrder order, Fn<void(ArchiveDeletedPage)> done);
void ReadArchiveDeletedView(not_null<Main::Session*> session, QString id,
	ArchiveStoreEpoch epoch, Fn<void(ArchiveDeletedPage)> done);
void CloseArchiveDeletedView(not_null<Main::Session*> session, QString id,
	ArchiveStoreEpoch epoch);
void PrepareArchivePurge(not_null<Main::Session*> session, ArchivePurgeKind kind,
	ArchivePurgeScope scope, Fn<void(DesktopArchivePurgePreview)> done);
void ExecuteArchivePurge(not_null<Main::Session*> session, ArchivePurgeConfirmation confirmation,
	std::shared_ptr<std::atomic<bool>> permitted, Fn<void(DesktopArchiveStatus)> done);
void RetryArchiveMaintenance(not_null<Main::Session*> session, ArchiveStoreEpoch epoch,
	std::shared_ptr<std::atomic<bool>> permitted, Fn<void(DesktopArchiveStatus)> done);
void OpenArchiveMedia(not_null<Main::Session*> session, ArchiveMediaRequest request,
	std::function<bool()> current,
	Fn<void(std::shared_ptr<ArchiveMediaAccess>)> done);

}

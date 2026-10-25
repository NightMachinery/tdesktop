#pragma once

#include "purple/purple_archive_crypto.h"
#include "purple/purple_archive_lifecycle.h"
#include "purple/purple_archive_media.h"
#include "purple/purple_archive_media_access.h"
#include "purple/purple_archive_staging.h"
#include "purple/purple_archive_sqlite.h"
#include <memory>

namespace Purple {

struct DesktopArchiveJournalAccess {
	QString root;
	ArchiveStoreEpoch epoch;
	ArchiveSqlSecurity security;
	ArchiveGeneration generation;
	uint64_t minimumOrder = 0;
	std::function<bool()> current;
	std::function<bool()> active;
};

enum class DesktopArchiveStatus {
	Ready,
	Disabled,
	Busy,
	Unreadable,
	ReadOnly,
	Invalid,
	IoError,
	RemovalPending,
	RetentionGap,
	Stale,
	SchemaMismatch
};
struct DesktopArchiveLoginAuthority {
	QByteArray nonce;
	QByteArray authKeys;
};
[[nodiscard]] std::optional<DesktopArchiveLoginAuthority> DesktopArchiveSelectLoginAuthority(
	const QByteArray &nonce, const QByteArray &storedKeys, const QByteArray &liveKeys);

[[nodiscard]] bool DesktopArchiveLoginKeysValid(const QByteArray &keys);
[[nodiscard]] bool DesktopArchiveLoginKeysOverlap(const QByteArray &a, const QByteArray &b);

struct DesktopArchiveDiscardToken {
	AccountKey account;
	QString profile;
	QByteArray profileIdentity;
	QByteArray directoryIdentity;
	QByteArray recordDigest;
	QByteArray fenceDigest;
	QString authority;
	QByteArray keys;
};

struct DesktopArchiveOptions {
	QString archiveRoot;
	QString profileRoot;
	AccountKey account;
	QString slot;
	QByteArray localKey;
	bool create = false;
	std::function<bool()> active;
	QString profileNamespace;
	QByteArray loginAuthority;
	QString removalAuthority;
	QString removalStore;
	std::function<bool()> authorityReady;
	std::function<bool()> maintenanceAllowed;
	std::shared_ptr<DesktopArchiveCrypto> revocableSecrets;
	QByteArray loginAuthKeyId;
	QByteArray loginAuthKeyIds;
};
struct DesktopArchiveCaptureResult {
	DesktopArchiveStatus status = DesktopArchiveStatus::Invalid;
	ArchivePlanStatus model = ArchivePlanStatus::Invalid;
	QString reason;
	bool retained = false;
};
struct DesktopArchiveHistoryPage {
	ArchiveStoreEpoch epoch;
	DesktopArchiveStatus status = DesktopArchiveStatus::Invalid;
	std::vector<ArchiveVersionSnapshot> versions;
	QString beforeOrder;
	uint64_t revision = 0;
	bool more = false;
	bool hasGap = false;
	ArchiveGeneration generation;
};

struct DesktopArchiveStagingCounters {
	uint64_t fenceReads = 0;
	uint64_t proofQueries = 0;
	uint64_t catalogueOpens = 0;
	size_t cachedStreams = 0;
};

class DesktopArchiveStore final {
  public:
	DesktopArchiveStore();
	~DesktopArchiveStore();
	[[nodiscard]] DesktopArchiveStatus Open(DesktopArchiveOptions options);
	[[nodiscard]] bool Close();
	[[nodiscard]] static std::optional<DesktopArchiveDiscardToken> PrepareDiscard(
		const DesktopArchiveOptions &options);
	[[nodiscard]] static bool DiscardAccount(DesktopArchiveOptions options,
		const DesktopArchiveDiscardToken &token, int maximumActions = 64);
	[[nodiscard]] bool Current() const;
	[[nodiscard]] bool MemoryCurrent() const;
	[[nodiscard]] DesktopArchiveStagingCounters StagingCountersForTest() const;
	[[nodiscard]] DesktopArchiveStatus status() const;
	[[nodiscard]] QString failure() const;
	[[nodiscard]] const ArchiveStoreEpoch &epoch() const;
	[[nodiscard]] const ArchiveSqlSecurity &security() const;
	[[nodiscard]] QString directory() const;
	[[nodiscard]] DesktopArchiveCaptureResult Capture(ArchiveEvent event);
	[[nodiscard]] std::optional<ArchiveSqlSlice> ReadSlice(const ArchiveMessageKey &key,
		const QString &event = {}, const QString &target = {}, const QString &lease = {},
		const QString &blob = {});
	[[nodiscard]] DesktopArchiveHistoryPage Versions(const ArchiveMessageKey &key,
		int limit = 64, const QString &beforeOrder = {},
		std::optional<uint64_t> revision = std::nullopt);
	[[nodiscard]] std::optional<std::vector<ArchiveMessageKey>> NonChannelMessages(
		int64 messageId);
	[[nodiscard]] std::optional<QString> ObservedMediaVersion(
		const ArchiveMessageKey &key, const QString &observationId,
		const QString &contentHash, const QString &identity);
	[[nodiscard]] bool CaptureLifecycle(const ArchiveEvent &preimage, const ArchiveStoreEpoch &captured);
	[[nodiscard]] std::optional<ArchiveStagingBinding> StageStream(const ArchiveMessageKey &key,
		const QString &version, const QString &identity, uint64_t size);
	[[nodiscard]] bool ResumeStreams(QString &cursor, bool &more);
	[[nodiscard]] bool StreamPart(const ArchiveStagingBinding &binding, uint64_t offset, const QByteArray &bytes,
		std::shared_ptr<std::atomic<bool>> active = {});
	void PruneStreams();
	[[nodiscard]] bool CopyMedia(const ArchiveMessageKey &key, const QString &version,
		const QString &identity, QIODevice &source, const DesktopArchiveMediaCopy &copy);
	[[nodiscard]] bool ReadMedia(const ArchiveMessageKey &key, const QString &version,
		const QString &identity, const std::function<bool(const QByteArray &)> &sink);
	[[nodiscard]] std::optional<DesktopArchiveMediaHandle> OpenMedia(
		const ArchiveMediaRequest &request,
		ArchiveLeaseRevisionChange *committed = nullptr);
	[[nodiscard]] bool MediaCurrent(const DesktopArchiveMediaHandle &handle);
	[[nodiscard]] std::optional<QByteArray> MediaRange(const DesktopArchiveMediaHandle &handle,
		uint64_t offset, uint32_t length);
	[[nodiscard]] bool ExportMedia(const DesktopArchiveMediaHandle &handle, const QString &path);
	[[nodiscard]] bool ReleaseMedia(const DesktopArchiveMediaHandle &handle,
		ArchiveLeaseRevisionChange *committed = nullptr);
	void RefreshMediaReaders();
	[[nodiscard]] std::optional<DesktopArchiveJournalAccess> JournalAccess();
	[[nodiscard]] bool UsageComplete() const;
	[[nodiscard]] bool Gap(const QString &id, const QString &reason, int64 observedAt);
	[[nodiscard]] std::optional<ArchiveUsage> Usage();
	[[nodiscard]] bool Remove(int maximumActions = 4096);
	[[nodiscard]] static bool Recover(const DesktopArchiveOptions &options);
	[[nodiscard]] static bool RemoveAccount(
		DesktopArchiveOptions options, int maximumActions = 4096);
	[[nodiscard]] static std::optional<AccountKey> AccountForSlot(
		const DesktopArchiveOptions &options, const QString &slot, bool* valid = nullptr);
	[[nodiscard]] bool Execute(const ArchiveSqlProgram &program);
	DesktopArchiveSqlite &database();

  private:
	friend class DesktopArchiveStream;
	void CountStagingProof();
	void CountStagingOpen();
	class Impl;
	std::unique_ptr<Impl> _impl;

};

}

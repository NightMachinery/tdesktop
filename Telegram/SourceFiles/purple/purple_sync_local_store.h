/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_config_payload.h"
#include "purple/purple_sync_local_state.h"
#include "purple/purple_sync_config_flow.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <functional>
#include <memory>
#include <optional>
#include <utility>

class QLockFile;

namespace Purple {

struct SyncCarryJournal;
[[nodiscard]] std::optional<SyncCarryJournal> InspectSyncCarryJournal(
	const QByteArray &bytes, const QByteArray &bindingToken);

enum class SyncStoreStatus {
	Disabled,
	Uninitialized,
	Ready,
	NoPending,
	LockBusy,
	LockError,
	IoError,
	InsecurePermissions,
	InvalidState,
	NewerState,
	PendingMissing,
	PendingMismatch,
	OrphanStage,
	InvalidRecord,
	InvalidTransition,
	Unconfirmed,
	CloneDetected,
	AccountUnbound,
	CleanupFailed,
	CarryPending,
	InvalidRetention,
	NewerRetention,
	BaseRecordTooLarge,
	RetentionFull,
	MissingLocalVersion,
	WrongThread,
	Changed,
	DurabilityFailed,
};

[[nodiscard]] SyncStoreStatus SyncCarryStartupAdmission(
	const QString &directory, const QByteArray &bindingToken);

struct SyncStoreResult {
	SyncStoreResult(
		SyncStoreStatus status,
		SyncLocalError stateError = SyncLocalError::None,
		SyncEnvelopeError envelopeError = SyncEnvelopeError::None,
		ConfigPayloadError payloadError = ConfigPayloadError::None,
		QByteArray staged = {},
		uint64_t seq = 0,
		SyncCloneVerdict cloneVerdict = SyncCloneVerdict::NoClone)
	: status(status)
	, stateError(stateError)
	, envelopeError(envelopeError)
	, payloadError(payloadError)
	, staged(std::move(staged))
	, seq(seq)
	, cloneVerdict(cloneVerdict) {
	}

	SyncStoreStatus status = SyncStoreStatus::InvalidTransition;
	SyncLocalError stateError = SyncLocalError::None;
	SyncEnvelopeError envelopeError = SyncEnvelopeError::None;
	ConfigPayloadError payloadError = ConfigPayloadError::None;
	QByteArray staged;
	uint64_t seq = 0;
	SyncCloneVerdict cloneVerdict = SyncCloneVerdict::NoClone;

	[[nodiscard]] explicit operator bool() const {
		return status == SyncStoreStatus::Ready;
	}
};

inline constexpr auto kSyncSettingsRecordStorageLimit = 256 * 1024;
inline constexpr auto kSyncRetainedBaseTotalLimit = 2 * 1024 * 1024;
inline constexpr auto kSyncRetainedBaseCountLimit = 16;

[[nodiscard]] SyncStoreStatus ValidateSyncRetainedConfig(const SyncLocalState &state);
[[nodiscard]] SyncStoreResult ReadSyncRetainedConfigBase(const SyncLocalState &state);
[[nodiscard]] SyncStoreStatus RetainSyncConfigRecord(
	SyncLocalState &state, const QByteArray &record);

[[nodiscard]] SyncStoreStatus EnsureSyncPrivateDirectory(const QString &path);
[[nodiscard]] SyncStoreStatus ReadSyncPrivateFile(
	const QString &path,
	qsizetype limit,
	QByteArray &bytes);
[[nodiscard]] SyncStoreStatus WriteSyncPrivateFile(
	const QString &path,
	const QByteArray &bytes);

[[nodiscard]] QByteArray SyncPathIdentity(const QString &path);

enum class SyncDurablePoint { BeforeWrite, PartialWrite, BeforeRename, AfterRename, FileBarrier, DirectoryBarrier };
using SyncDurableFault = std::function<bool(SyncDurablePoint)>;

struct SyncPrivateSnapshot {
	SyncStoreStatus status = SyncStoreStatus::IoError;
	QByteArray identity;
	QByteArray parentIdentity;
	QByteArray bytes;
	[[nodiscard]] bool absent() const { return status == SyncStoreStatus::Ready && identity == "missing"; }
};

[[nodiscard]] SyncPrivateSnapshot CaptureSyncPrivateFile(const QString &path, qsizetype limit);
[[nodiscard]] SyncStoreStatus WriteSyncDurableFile(
	const QString &path, const QByteArray &bytes,
	const SyncPrivateSnapshot &expected, const SyncDurableFault &fault = {});
[[nodiscard]] SyncStoreStatus ConfirmSyncDurableFile(
	const QString &path, const SyncPrivateSnapshot &expected,
	const SyncDurableFault &fault = {});
[[nodiscard]] SyncStoreStatus SyncPrivateDirectoryBarrier(const QString &path);
[[nodiscard]] bool SyncStoreWorkerThread();

struct SyncRecoveryRead {
	SyncStoreStatus status = SyncStoreStatus::IoError;
	SyncRecoveryStore store;
	std::map<QString, QByteArray> identities;
};

[[nodiscard]] SyncRecoveryRead ReadSyncRecoveryStore(
	const AccountFiles &files, std::optional<AccountKey> directoryAccount);

class SyncChatCarryStore;

class SyncLocalStore final {
public:
	explicit SyncLocalStore(QString syncRoot);
	~SyncLocalStore();

	SyncLocalStore(const SyncLocalStore &) = delete;
	SyncLocalStore &operator=(const SyncLocalStore &) = delete;

	[[nodiscard]] SyncStoreResult Open(bool optedIn, const QByteArray &bindingToken = {});
	[[nodiscard]] SyncStoreResult Initialize(const SyncLocalState &initial);
	[[nodiscard]] SyncStoreResult ReadPendingConfig();
	[[nodiscard]] SyncStoreResult StageConfig(
		const QByteArray &canonicalRecord,
		const SyncLocalConfigState &nextConfigData,
		const QByteArray &accountBindingToken);
	[[nodiscard]] SyncStoreResult ConfirmConfigReadBack(
		const QByteArray &serverRecord,
		const QString &currentDevice,
		int32_t messageId,
		PeerToken chat = {});
	[[nodiscard]] SyncStoreResult CommitConfigData(
		const SyncLocalConfigState &next,
		const QByteArray &accountBindingToken,
		const QByteArray &baseRecord = {});
	[[nodiscard]] SyncStoreStatus CheckConfigRetention(
		const SyncLocalConfigState &next, const QByteArray &record = {});
	[[nodiscard]] SyncStoreStatus SupplyRetainedConfigBase(const QByteArray &record);
	[[nodiscard]] SyncStoreStatus CheckpointRetainedConfigBases(const SyncDurableFault &fault = {});
	[[nodiscard]] SyncStoreResult RemoveAbsentConfigMessage(
		int32_t messageId,
		const QByteArray &record,
		const QByteArray &accountBindingToken,
		PeerToken chat = {});
	[[nodiscard]] const SyncLocalState *state() const;
	[[nodiscard]] SyncStoreStatus status() const;
	[[nodiscard]] const QString &directory() const;

private:
	friend class SyncChatCarryStore;
	friend class DesktopSyncAutoStore;
	[[nodiscard]] SyncStoreResult OpenForCarry(const QByteArray &bindingToken);
	[[nodiscard]] SyncStoreResult OpenChecked(
		bool optedIn, const QByteArray &bindingToken, bool carryOwner);
	[[nodiscard]] SyncStoreStatus PrepareRetainedConfig(
		SyncLocalState &next, const QByteArray &record = {});
	[[nodiscard]] QString StatePath() const;
	[[nodiscard]] QString PendingPath(uint64_t seq) const;
	[[nodiscard]] SyncStoreResult ReadPendingConfigLocked();
	[[nodiscard]] SyncStoreResult SetFailure(SyncStoreResult result);

	QString _root;
	std::unique_ptr<QLockFile> _lock;
	std::optional<SyncLocalState> _state;
	SyncStoreStatus _status = SyncStoreStatus::Disabled;
	bool _opened = false;
	bool _carryOwner = false;
	QByteArray _retainedBootstrap;
};

}

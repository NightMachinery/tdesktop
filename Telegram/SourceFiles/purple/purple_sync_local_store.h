/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_config_payload.h"
#include "purple/purple_sync_local_state.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <memory>
#include <optional>
#include <utility>

class QLockFile;

namespace Purple {

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
	CleanupFailed,
};

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

class SyncLocalStore final {
public:
	explicit SyncLocalStore(QString syncRoot);
	~SyncLocalStore();

	SyncLocalStore(const SyncLocalStore &) = delete;
	SyncLocalStore &operator=(const SyncLocalStore &) = delete;

	[[nodiscard]] SyncStoreResult Open(bool optedIn);
	[[nodiscard]] SyncStoreResult Initialize(const SyncLocalState &initial);
	[[nodiscard]] SyncStoreResult ReadPendingConfig();
	[[nodiscard]] SyncStoreResult StageConfig(
		const QByteArray &canonicalRecord,
		const SyncLocalConfigState &nextConfigData);
	[[nodiscard]] SyncStoreResult ConfirmConfigReadBack(
		const QByteArray &serverRecord,
		const QString &currentDevice,
		int32_t messageId);
	[[nodiscard]] const SyncLocalState *state() const;
	[[nodiscard]] SyncStoreStatus status() const;

private:
	[[nodiscard]] QString StatePath() const;
	[[nodiscard]] QString PendingPath(uint64_t seq) const;
	[[nodiscard]] SyncStoreResult ReadPendingConfigLocked();
	[[nodiscard]] SyncStoreResult SetFailure(SyncStoreResult result);

	QString _root;
	std::unique_ptr<QLockFile> _lock;
	std::optional<SyncLocalState> _state;
	SyncStoreStatus _status = SyncStoreStatus::Disabled;
	bool _opened = false;
};

}

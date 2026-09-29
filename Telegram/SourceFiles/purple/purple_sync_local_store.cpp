/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_local_store.h"

#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QLockFile>
#include <QtCore/QSaveFile>
#include <QtCore/QStringList>

#include <utility>

namespace Purple {
namespace {

constexpr auto kStateBytes = 4 * 1024 * 1024;
constexpr auto kConfigBytes = 256 * 1024;

[[nodiscard]] bool PrivatePermissions(const QString &path) {
	const auto info = QFileInfo(path);
	if (info.isSymLink()) {
		return false;
	}
#ifdef Q_OS_UNIX
	constexpr auto broader = QFileDevice::ReadGroup
		| QFileDevice::WriteGroup
		| QFileDevice::ExeGroup
		| QFileDevice::ReadOther
		| QFileDevice::WriteOther
		| QFileDevice::ExeOther;
	return !(info.permissions() & broader);
#else
	return true;
#endif
}

[[nodiscard]] SyncStoreStatus EnsurePrivateDirectory(const QString &path) {
	const auto usable = [&] {
		if (!PrivatePermissions(path)) {
			return false;
		}
#ifdef Q_OS_UNIX
		constexpr auto required = QFileDevice::ReadOwner
			| QFileDevice::WriteOwner | QFileDevice::ExeOwner;
		return (QFileInfo(path).permissions() & required) == required;
#else
		return true;
#endif
	};
	const auto info = QFileInfo(path);
	if (info.exists() || info.isSymLink()) {
		if (!info.isDir()) {
			return SyncStoreStatus::IoError;
		}
		return usable()
			? SyncStoreStatus::Ready
			: SyncStoreStatus::InsecurePermissions;
	}
#ifdef Q_OS_UNIX
	constexpr auto permissions = QFileDevice::ReadOwner
		| QFileDevice::WriteOwner
		| QFileDevice::ExeOwner;
	if (!QDir().mkdir(path, permissions)) {
		return SyncStoreStatus::IoError;
	}
#else
	if (!QDir().mkdir(path)) {
		return SyncStoreStatus::IoError;
	}
#endif
	return usable()
		? SyncStoreStatus::Ready
		: SyncStoreStatus::InsecurePermissions;
}

[[nodiscard]] SyncStoreStatus ReadExact(
		const QString &path,
		qsizetype limit,
		QByteArray &bytes) {
	const auto info = QFileInfo(path);
	if (!info.exists()) {
		return SyncStoreStatus::PendingMissing;
	}
	if (!info.isFile() || info.size() > limit) {
		return SyncStoreStatus::PendingMismatch;
	}
	if (!PrivatePermissions(path)) {
		return SyncStoreStatus::InsecurePermissions;
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return SyncStoreStatus::IoError;
	}
	bytes = file.readAll();
	return (file.error() == QFileDevice::NoError && bytes.size() == info.size())
		? SyncStoreStatus::Ready
		: SyncStoreStatus::IoError;
}

[[nodiscard]] SyncStoreStatus WriteExact(
		const QString &path,
		const QByteArray &bytes) {
	auto file = QSaveFile(path);
	file.setDirectWriteFallback(false);
	if (!file.open(QIODevice::WriteOnly)) {
		return SyncStoreStatus::IoError;
	}
#ifdef Q_OS_UNIX
	constexpr auto permissions = QFileDevice::ReadOwner
		| QFileDevice::WriteOwner;
	if (!file.setPermissions(permissions)) {
		return SyncStoreStatus::InsecurePermissions;
	}
#endif
	if (file.write(bytes) != bytes.size() || !file.commit()) {
		return SyncStoreStatus::IoError;
	}
	if (!PrivatePermissions(path)) {
		return SyncStoreStatus::InsecurePermissions;
	}
	auto readBack = QByteArray();
	const auto status = ReadExact(path, bytes.size(), readBack);
	return (status == SyncStoreStatus::Ready && readBack == bytes)
		? SyncStoreStatus::Ready
		: SyncStoreStatus::IoError;
}

[[nodiscard]] SyncStoreStatus CheckStages(
		const QString &pendingPath,
		const SyncLocalState *state) {
	auto obsolete = QStringList();
	auto files = QDirIterator(
		pendingPath,
		QDir::AllEntries | QDir::Hidden | QDir::System
			| QDir::NoDotAndDotDot);
	while (files.hasNext()) {
		const auto path = files.next();
		const auto name = QFileInfo(path).fileName();
		if (!name.startsWith(u"config-"_q)
			|| !name.endsWith(u".json"_q)) {
			continue;
		}
		if (!state) {
			return SyncStoreStatus::OrphanStage;
		}
		const auto middle = name.mid(7, name.size() - 12);
		auto valid = false;
		const auto seq = middle.toULongLong(&valid, 10);
		if (!valid || seq == 0 || seq > 9007199254740991ULL
			|| QString::number(seq) != middle) {
			return SyncStoreStatus::OrphanStage;
		}
		if (seq == state->config.pendingSeq) {
			continue;
		}
		if (seq > state->config.seq) {
			return SyncStoreStatus::OrphanStage;
		}
		auto bytes = QByteArray();
		const auto readStatus = ReadExact(path, kConfigBytes, bytes);
		if (readStatus != SyncStoreStatus::Ready) {
			return readStatus;
		}
		const auto parsed = ParseSyncEnvelope(bytes);
		const auto inspected = InspectConfigPayload(parsed);
		const auto canonical = parsed
			? SerializeSyncEnvelope(parsed.envelope)
			: SyncEnvelopeWriteResult();
		const auto document = parsed.envelope.document;
		if (!canonical || canonical.canonical != bytes
			|| inspected.status != ConfigPayloadStatus::Valid
			|| document.value(u"space"_q).toString() != state->space
			|| document.value(u"writer"_q).toObject()
				.value(u"install"_q).toString() != state->install
			|| uint64_t(document.value(u"seq"_q).toDouble()) != seq
			|| (seq == state->config.seq
				&& (state->config.pendingSeq != 0
					|| document.value(u"payload_sha256"_q).toString()
						!= state->config.ownHash
					|| inspected.version.key != state->configData.base
					|| inspected.version.lineage
						!= state->configData.baseLineage))) {
			return SyncStoreStatus::OrphanStage;
		}
		obsolete.push_back(path);
	}
	for (const auto &path : obsolete) {
		if (!QFile::remove(path)) {
			return SyncStoreStatus::CleanupFailed;
		}
	}
	return SyncStoreStatus::Ready;
}

[[nodiscard]] SyncStoreResult RecordError(
		const SyncEnvelopeParseResult &parsed,
		const ConfigPayloadInspection &inspected) {
	return {
		SyncStoreStatus::InvalidRecord,
		SyncLocalError::None,
		parsed.error,
		inspected.error,
	};
}

}

SyncLocalStore::SyncLocalStore(QString syncRoot)
: _root(std::move(syncRoot)) {
}

SyncLocalStore::~SyncLocalStore() = default;

SyncStoreResult SyncLocalStore::SetFailure(SyncStoreResult result) {
	_status = result.status;
	return result;
}

QString SyncLocalStore::StatePath() const {
	return _root + u"/state.json"_q;
}

QString SyncLocalStore::PendingPath(uint64_t seq) const {
	return _root + u"/pending/config-"_q
		+ QString::number(seq) + u".json"_q;
}

SyncStoreResult SyncLocalStore::Open(bool optedIn) {
	if (_opened) {
		return { SyncStoreStatus::InvalidTransition };
	}
	if (!optedIn) {
		return { SyncStoreStatus::Disabled };
	}
	_opened = true;
	if (_root.isEmpty()
		|| !QDir().mkpath(QFileInfo(_root).absolutePath())) {
		return SetFailure({ SyncStoreStatus::IoError });
	}
	const auto rootStatus = EnsurePrivateDirectory(_root);
	if (rootStatus != SyncStoreStatus::Ready) {
		return SetFailure({ rootStatus });
	}
	_lock = std::make_unique<QLockFile>(_root + u"/lock"_q);
	_lock->setStaleLockTime(0);
	if (!_lock->tryLock(0)) {
		return SetFailure({
			(_lock->error() == QLockFile::LockFailedError)
				? SyncStoreStatus::LockBusy
				: SyncStoreStatus::LockError,
		});
	}
	const auto pendingStatus = EnsurePrivateDirectory(_root + u"/pending"_q);
	if (pendingStatus != SyncStoreStatus::Ready) {
		return SetFailure({ pendingStatus });
	}
	const auto stateInfo = QFileInfo(StatePath());
	if (!stateInfo.exists() && !stateInfo.isSymLink()) {
		const auto stageStatus = CheckStages(
			_root + u"/pending"_q, nullptr);
		return SetFailure({ stageStatus == SyncStoreStatus::Ready
			? SyncStoreStatus::Uninitialized : stageStatus });
	}
	auto bytes = QByteArray();
	const auto readStatus = ReadExact(StatePath(), kStateBytes, bytes);
	if (readStatus != SyncStoreStatus::Ready) {
		return SetFailure({ readStatus == SyncStoreStatus::PendingMismatch
			? SyncStoreStatus::InvalidState
			: readStatus == SyncStoreStatus::PendingMissing
			? SyncStoreStatus::IoError
			: readStatus });
	}
	const auto parsed = ParseSyncLocalState(bytes);
	if (!parsed) {
		return SetFailure({
			(parsed.status == SyncLocalStatus::NewerVersion)
				? SyncStoreStatus::NewerState
				: SyncStoreStatus::InvalidState,
			parsed.error,
		});
	}
	_state = parsed.state;
	_status = SyncStoreStatus::Ready;
	if (_state->config.pendingSeq != 0) {
		const auto pending = ReadPendingConfigLocked();
		if (!pending) {
			return SetFailure(pending);
		}
	}
	const auto stageStatus = CheckStages(
		_root + u"/pending"_q, &*_state);
	if (stageStatus != SyncStoreStatus::Ready) {
		return SetFailure({ stageStatus });
	}
	return { SyncStoreStatus::Ready };
}

SyncStoreResult SyncLocalStore::Initialize(const SyncLocalState &initial) {
	if (_status != SyncStoreStatus::Uninitialized
		|| !_lock || !_lock->isLocked()
		|| QFileInfo(StatePath()).exists()
		|| QFileInfo(StatePath()).isSymLink()
		|| initial.config.seq != 0
		|| initial.library.seq != 0) {
		return { SyncStoreStatus::InvalidTransition };
	}
	const auto serialized = SerializeSyncLocalState(initial);
	if (!serialized) {
		return {
			SyncStoreStatus::InvalidState,
			serialized.error,
		};
	}
	const auto writeStatus = WriteExact(StatePath(), serialized.canonical);
	if (writeStatus != SyncStoreStatus::Ready) {
		return SetFailure({ writeStatus });
	}
	_state = initial;
	_status = SyncStoreStatus::Ready;
	return { SyncStoreStatus::Ready };
}

SyncStoreResult SyncLocalStore::ReadPendingConfigLocked() {
	if (!_state || _state->config.pendingSeq == 0) {
		return { SyncStoreStatus::NoPending };
	}
	auto bytes = QByteArray();
	const auto readStatus = ReadExact(
		PendingPath(_state->config.pendingSeq), kConfigBytes, bytes);
	if (readStatus != SyncStoreStatus::Ready) {
		return { readStatus };
	}
	const auto parsed = ParseSyncEnvelope(bytes);
	const auto inspected = InspectConfigPayload(parsed);
	if (!parsed || inspected.status != ConfigPayloadStatus::Valid) {
		return { SyncStoreStatus::PendingMismatch,
			SyncLocalError::None, parsed.error, inspected.error };
	}
	const auto canonical = SerializeSyncEnvelope(parsed.envelope);
	const auto document = parsed.envelope.document;
	if (!canonical || canonical.canonical != bytes
		|| document.value(u"space"_q).toString() != _state->space
		|| document.value(u"writer"_q).toObject()
			.value(u"install"_q).toString() != _state->install
		|| uint64_t(document.value(u"seq"_q).toDouble())
			!= _state->config.pendingSeq
		|| document.value(u"payload_sha256"_q).toString()
			!= _state->config.ownHash
		|| inspected.version.key != _state->configData.pending) {
		return { SyncStoreStatus::PendingMismatch };
	}
	return { SyncStoreStatus::Ready,
		SyncLocalError::None, SyncEnvelopeError::None,
		ConfigPayloadError::None, bytes, _state->config.pendingSeq };
}

SyncStoreResult SyncLocalStore::ReadPendingConfig() {
	if (_status != SyncStoreStatus::Ready) {
		return { _status };
	}
	const auto result = ReadPendingConfigLocked();
	return (result.status == SyncStoreStatus::PendingMissing
		|| result.status == SyncStoreStatus::PendingMismatch
		|| result.status == SyncStoreStatus::InsecurePermissions
		|| result.status == SyncStoreStatus::IoError)
		? SetFailure(result)
		: result;
}

SyncStoreResult SyncLocalStore::StageConfig(
		const QByteArray &canonicalRecord,
		const SyncLocalConfigState &nextConfigData) {
	if (_status != SyncStoreStatus::Ready || !_state) {
		return { SyncStoreStatus::InvalidTransition };
	}
	const auto parsed = ParseSyncEnvelope(canonicalRecord);
	const auto inspected = InspectConfigPayload(parsed);
	if (!parsed || inspected.status != ConfigPayloadStatus::Valid) {
		return RecordError(parsed, inspected);
	}
	const auto canonical = SerializeSyncEnvelope(parsed.envelope);
	const auto document = parsed.envelope.document;
	if (!canonical || canonical.canonical != canonicalRecord
		|| document.value(u"space"_q).toString() != _state->space
		|| document.value(u"writer"_q).toObject()
			.value(u"install"_q).toString() != _state->install
		|| uint64_t(document.value(u"seq"_q).toDouble())
			!= _state->config.seq + 1
		|| nextConfigData.pending != inspected.version.key) {
		return { SyncStoreStatus::InvalidRecord };
	}
	const auto hash = document.value(u"payload_sha256"_q).toString();
	const auto reserved = ReserveSyncSeq(
		*_state, SyncLocalStream::Config, hash);
	if (!reserved) {
		return { SyncStoreStatus::InvalidTransition };
	}
	auto next = reserved.state;
	next.configData = nextConfigData;
	const auto issued = AppendIssuedConfigRecord(next, canonicalRecord);
	if (!issued) {
		return { issued.error == SyncIssueError::InvalidState
			? SyncStoreStatus::InvalidState : SyncStoreStatus::InvalidRecord };
	}
	next = issued.state;
	const auto serialized = SerializeSyncLocalState(next);
	if (!serialized) {
		return { SyncStoreStatus::InvalidState, serialized.error };
	}
	const auto path = PendingPath(reserved.seq);
	const auto stageInfo = QFileInfo(path);
	if (stageInfo.exists() || stageInfo.isSymLink()) {
		return SetFailure({ SyncStoreStatus::OrphanStage });
	}
	const auto stageStatus = WriteExact(path, canonicalRecord);
	if (stageStatus != SyncStoreStatus::Ready) {
		return SetFailure({ stageStatus });
	}
	const auto stateStatus = WriteExact(StatePath(), serialized.canonical);
	if (stateStatus != SyncStoreStatus::Ready) {
		return SetFailure({ stateStatus });
	}
	_state = std::move(next);
	const auto pending = ReadPendingConfigLocked();
	if (!pending) {
		return SetFailure(pending);
	}
	return pending;
}

SyncStoreResult SyncLocalStore::ConfirmConfigReadBack(
		const QByteArray &serverRecord,
		const QString &currentDevice,
		int32_t messageId) {
	if (_status != SyncStoreStatus::Ready || !_state) {
		return { SyncStoreStatus::InvalidTransition };
	}
	if (_state->config.pendingSeq == 0) {
		return { SyncStoreStatus::NoPending };
	}
	if (messageId <= 0) {
		return { SyncStoreStatus::InvalidRecord };
	}
	const auto pending = ReadPendingConfigLocked();
	if (!pending) {
		return SetFailure(pending);
	}
	const auto parsed = ParseSyncEnvelope(serverRecord);
	const auto inspected = InspectConfigPayload(parsed);
	if (!parsed || inspected.status != ConfigPayloadStatus::Valid) {
		return RecordError(parsed, inspected);
	}
	const auto canonical = SerializeSyncEnvelope(parsed.envelope);
	const auto document = parsed.envelope.document;
	if (!canonical || canonical.canonical != serverRecord
		|| document.value(u"space"_q).toString() != _state->space
		|| document.value(u"writer"_q).toObject()
			.value(u"install"_q).toString() != _state->install) {
		return { SyncStoreStatus::InvalidRecord };
	}
	const auto stagedRecord = ParseSyncEnvelope(pending.staged);
	const auto stagedVersion = InspectConfigPayload(stagedRecord).version;
	const auto observation = OwnRecordObservation{
		OwnRecordObservationKind::Present,
		uint64_t(document.value(u"seq"_q).toDouble()),
		document.value(u"payload_sha256"_q).toString(),
	};
	const auto confirmation = Purple::ConfirmConfigReadBack(
		*_state, currentDevice, observation, stagedVersion);
	if (confirmation.verdict != SyncCloneVerdict::NoClone) {
		return SetFailure({ SyncStoreStatus::CloneDetected,
			SyncLocalError::None, SyncEnvelopeError::None,
			ConfigPayloadError::None, {}, 0, confirmation.verdict });
	}
	if (!confirmation.changed) {
		return { SyncStoreStatus::Unconfirmed };
	}
	if (inspected.version.key != stagedVersion.key
		|| inspected.version.lineage != stagedVersion.lineage) {
		return { SyncStoreStatus::Unconfirmed };
	}
	if (serverRecord != pending.staged) {
		return { SyncStoreStatus::Unconfirmed };
	}
	const auto recorded = RecordConfirmedOwnConfigMessage(
		confirmation.state, messageId, serverRecord);
	if (!recorded) {
		return { recorded.error == SyncOwnMessageError::InvalidState
			? SyncStoreStatus::InvalidState : SyncStoreStatus::InvalidRecord };
	}
	const auto serialized = SerializeSyncLocalState(recorded.state);
	if (!serialized) {
		return { SyncStoreStatus::InvalidState, serialized.error };
	}
	const auto stateStatus = WriteExact(StatePath(), serialized.canonical);
	if (stateStatus != SyncStoreStatus::Ready) {
		return SetFailure({ stateStatus });
	}
	_state = recorded.state;
	if (!QFile::remove(PendingPath(observation.seq))) {
		return SetFailure({ SyncStoreStatus::CleanupFailed });
	}
	const auto cleanupStatus = CheckStages(
		_root + u"/pending"_q, &*_state);
	if (cleanupStatus != SyncStoreStatus::Ready) {
		return SetFailure({ cleanupStatus });
	}
	return { SyncStoreStatus::Ready,
		SyncLocalError::None, SyncEnvelopeError::None,
		ConfigPayloadError::None, {}, observation.seq };
}

const SyncLocalState *SyncLocalStore::state() const {
	return _status == SyncStoreStatus::Ready && _state
		? &*_state : nullptr;
}

SyncStoreStatus SyncLocalStore::status() const {
	return _status;
}

}

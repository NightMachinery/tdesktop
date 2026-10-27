/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_local_store.h"

#include "purple/purple_sync_chat_scope.h"
#include "purple/purple_sync_config_flow.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QLockFile>
#include <QtCore/QSaveFile>
#include <QtCore/QStringList>
#include <QtCore/QThread>

#include <set>
#include <utility>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#else
#include <sys/stat.h>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Purple {
namespace {

constexpr auto kStateBytes = 4 * 1024 * 1024;
constexpr auto kConfigBytes = kSyncSettingsRecordStorageLimit;

constexpr auto kRetainedField = u"retained_config_bases";

struct RetainedRecords {
	SyncStoreStatus status = SyncStoreStatus::Ready;
	QJsonObject object;
	QJsonArray entries;
	qsizetype total = 0;
};

[[nodiscard]] bool MatchesBase(const SyncLocalState &state, const QByteArray &record) {
	const auto payload = InspectConfigPayload(ParseSyncEnvelope(record));
	return payload.status == ConfigPayloadStatus::Valid
		&& payload.version.key == state.configData.base
		&& payload.version.lineage == state.configData.baseLineage;
}

[[nodiscard]] bool ValidRetainedRecord(const SyncLocalState &state, const QByteArray &record) {
	const auto parsed = ParseSyncEnvelope(record);
	const auto encoded = parsed ? SerializeSyncEnvelope(parsed.envelope) : SyncEnvelopeWriteResult();
	return encoded && encoded.canonical == record
		&& parsed.envelope.document.value(u"space"_q).toString() == state.space
		&& InspectConfigPayload(parsed).status == ConfigPayloadStatus::Valid;
}

[[nodiscard]] RetainedRecords RetainedOf(const SyncLocalState &state) {
	auto result = RetainedRecords();
	const auto value = state.preserved.value(QString::fromUtf16(kRetainedField));
	if (value.isUndefined()) { return result; }
	if (!value.isObject()) { result.status = SyncStoreStatus::InvalidRetention; return result; }
	result.object = value.toObject();
	const auto version = result.object.value(u"version"_q);
	if (version.isDouble() && version.toDouble() > 1) {
		result.status = SyncStoreStatus::NewerRetention; return result;
	}
	if (!version.isDouble() || version.toDouble() != 1
		|| !result.object.value(u"records"_q).isArray()) {
		result.status = SyncStoreStatus::InvalidRetention; return result;
	}
	result.entries = result.object.value(u"records"_q).toArray();
	if (result.entries.size() > kSyncRetainedBaseCountLimit) {
		result.status = SyncStoreStatus::RetentionFull; return result;
	}
	auto hashes = std::set<QString>();
	for (const auto &entry : result.entries) {
		const auto object = entry.toObject();
		const auto text = object.value(u"record"_q);
		const auto hash = object.value(u"sha256"_q);
		if (!entry.isObject() || !text.isString() || !hash.isString()) {
			result.status = SyncStoreStatus::InvalidRetention; return result;
		}
		const auto bytes = text.toString().toUtf8();
		if (bytes.size() > kSyncSettingsRecordStorageLimit) {
			result.status = SyncStoreStatus::BaseRecordTooLarge; return result;
		}
		result.total += bytes.size();
		if (result.total > kSyncRetainedBaseTotalLimit) {
			result.status = SyncStoreStatus::RetentionFull; return result;
		}
		if (hash.toString() != QString::fromLatin1(QCryptographicHash::hash(bytes,
			QCryptographicHash::Sha256).toHex()) || !hashes.insert(hash.toString()).second
			|| !ValidRetainedRecord(state, bytes)) {
			result.status = SyncStoreStatus::InvalidRetention; return result;
		}
	}
	return result;
}

[[nodiscard]] bool PrivatePermissions(const QString &path) {
	const auto info = QFileInfo(path);
	if (info.isSymLink() || (info.isFile() && SyncPathIdentity(path).isEmpty())) {
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

[[nodiscard]] bool ReadablePrivateDirectory(const QFileInfo &info) {
	if (!info.isDir() || !info.isReadable() || !PrivatePermissions(info.filePath())) { return false; }
#ifdef Q_OS_UNIX
	constexpr auto required = QFileDevice::ReadOwner | QFileDevice::ExeOwner;
	return (info.permissions() & required) == required;
#else
	return true;
#endif
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

struct ConfigReservation {
	SyncStoreResult result;
	SyncLocalState state;
	QString key;
};

[[nodiscard]] ConfigReservation ReserveConfigRecord(
		const SyncLocalState &state,
		const QByteArray &record) {
	const auto parsed = ParseSyncEnvelope(record);
	const auto inspected = InspectConfigPayload(parsed);
	if (!parsed || inspected.status != ConfigPayloadStatus::Valid) {
		return { RecordError(parsed, inspected) };
	}
	const auto canonical = SerializeSyncEnvelope(parsed.envelope);
	const auto document = parsed.envelope.document;
	if (!canonical || canonical.canonical != record
		|| document.value(u"space"_q).toString() != state.space
		|| document.value(u"writer"_q).toObject()
			.value(u"install"_q).toString() != state.install
		|| uint64_t(document.value(u"seq"_q).toDouble())
			!= state.config.seq + 1) {
		return { { SyncStoreStatus::InvalidRecord } };
	}
	const auto reserved = ReserveSyncSeq(
		state,
		SyncLocalStream::Config,
		document.value(u"payload_sha256"_q).toString());
	if (!reserved) {
		return { { SyncStoreStatus::InvalidTransition } };
	}
	auto next = reserved.state;
	next.configData.pending = inspected.version.key;
	const auto issued = AppendIssuedConfigRecord(next, record);
	if (!issued) {
		return { { (issued.error == SyncIssueError::InvalidState)
			? SyncStoreStatus::InvalidState
			: SyncStoreStatus::InvalidRecord } };
	}
	return {
		{ SyncStoreStatus::Ready },
		issued.state,
		inspected.version.key,
	};
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
		const auto unreserved = (seq > state->config.seq);
		if (unreserved && seq != state->config.seq + 1) {
			return SyncStoreStatus::OrphanStage;
		}
		auto bytes = QByteArray();
		const auto readStatus = ReadSyncPrivateFile(
			path,
			kConfigBytes,
			bytes);
		if (readStatus != SyncStoreStatus::Ready) {
			return unreserved ? SyncStoreStatus::OrphanStage : readStatus;
		}
		if (unreserved) {
			if (!ReserveConfigRecord(*state, bytes).result) {
				return SyncStoreStatus::OrphanStage;
			}
			if (MatchesBase(*state, bytes) && !ReadSyncRetainedConfigBase(*state)) {
				return SyncStoreStatus::MissingLocalVersion;
			}
			obsolete.push_back(path);
			continue;
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
		if (inspected.version.key == state->configData.base
			&& inspected.version.lineage == state->configData.baseLineage
			&& !ReadSyncRetainedConfigBase(*state)) {
			return SyncStoreStatus::MissingLocalVersion;
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

}

SyncStoreStatus ValidateSyncRetainedConfig(const SyncLocalState &state) {
	return RetainedOf(state).status;
}

SyncStoreResult ReadSyncRetainedConfigBase(const SyncLocalState &state) {
	const auto retained = RetainedOf(state);
	if (retained.status != SyncStoreStatus::Ready) { return { retained.status }; }
	for (const auto &entry : retained.entries) {
		const auto record = entry.toObject().value(u"record"_q).toString().toUtf8();
		if (MatchesBase(state, record)) { return { SyncStoreStatus::Ready,
			SyncLocalError::None, SyncEnvelopeError::None, ConfigPayloadError::None, record }; }
	}
	return { SyncStoreStatus::MissingLocalVersion };
}

SyncStoreStatus RetainSyncConfigRecord(SyncLocalState &state, const QByteArray &record) {
	auto retained = RetainedOf(state);
	if (retained.status != SyncStoreStatus::Ready) { return retained.status; }
	if (record.size() > kSyncSettingsRecordStorageLimit) { return SyncStoreStatus::BaseRecordTooLarge; }
	if (!ValidRetainedRecord(state, record)) { return SyncStoreStatus::InvalidRecord; }
	const auto hash = QString::fromLatin1(QCryptographicHash::hash(record, QCryptographicHash::Sha256).toHex());
	for (const auto &entry : retained.entries) {
		if (entry.toObject().value(u"sha256"_q).toString() == hash) { return SyncStoreStatus::Ready; }
	}
	if (retained.entries.size() == kSyncRetainedBaseCountLimit
		|| retained.total + record.size() > kSyncRetainedBaseTotalLimit) {
		return SyncStoreStatus::RetentionFull;
	}
	retained.entries.append(QJsonObject{
		{ u"sha256"_q, hash }, { u"record"_q, QString::fromUtf8(record) } });
	retained.object.insert(u"version"_q, 1);
	retained.object.insert(u"records"_q, retained.entries);
	auto next = state;
	next.preserved.insert(QString::fromUtf16(kRetainedField), retained.object);
	if (!SerializeSyncLocalState(next)) { return SyncStoreStatus::RetentionFull; }
	state = std::move(next);
	return SyncStoreStatus::Ready;
}

SyncStoreStatus SyncLocalStore::CheckConfigRetention(
		const SyncLocalConfigState &config, const QByteArray &record) {
	if (_status != SyncStoreStatus::Ready || !_state || _carryOwner) {
		return SyncStoreStatus::InvalidTransition;
	}
	auto next = *_state;
	next.configData = config;
	return PrepareRetainedConfig(next, record);
}

SyncStoreStatus SyncLocalStore::SupplyRetainedConfigBase(const QByteArray &record) {
	if (_status != SyncStoreStatus::Ready || !_state || _carryOwner) {
		return SyncStoreStatus::InvalidTransition;
	}
	if (record.size() > kSyncSettingsRecordStorageLimit) { return SyncStoreStatus::BaseRecordTooLarge; }
	if (!ValidRetainedRecord(*_state, record) || !MatchesBase(*_state, record)) {
		return SyncStoreStatus::MissingLocalVersion;
	}
	_retainedBootstrap = record;
	return SyncStoreStatus::Ready;
}

SyncStoreStatus SyncLocalStore::PrepareRetainedConfig(
		SyncLocalState &next, const QByteArray &record) {
	if (!_state) { return SyncStoreStatus::InvalidState; }
	for (const auto &bytes : { _retainedBootstrap, record }) {
		if (!bytes.isEmpty()) {
			const auto status = RetainSyncConfigRecord(next, bytes);
			if (status != SyncStoreStatus::Ready) { return status; }
		}
	}
	const auto status = ValidateSyncRetainedConfig(next);
	if (status != SyncStoreStatus::Ready) { return status; }
	auto before = next;
	before.configData = _state->configData;
	if ((!before.configData.base.isEmpty() && !ReadSyncRetainedConfigBase(before))
		|| (!next.configData.base.isEmpty() && !ReadSyncRetainedConfigBase(next))) {
		return SyncStoreStatus::MissingLocalVersion;
	}
	return SyncStoreStatus::Ready;
}

SyncStoreStatus EnsureSyncPrivateDirectory(const QString &path) {
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

QByteArray SyncPathIdentity(const QString &path) {
	const auto info = QFileInfo(path);
	if (info.isSymLink()) { return {}; }
#ifdef Q_OS_WIN
	if (!info.exists()) {
		const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
		const auto error = GetLastError();
		return attributes == INVALID_FILE_ATTRIBUTES && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
			? QByteArray("missing") : QByteArray();
	}
	const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), 0,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (handle == INVALID_HANDLE_VALUE) { return {}; }
	auto value = BY_HANDLE_FILE_INFORMATION();
	const auto ok = GetFileInformationByHandle(handle, &value);
	CloseHandle(handle);
	if (!ok || (value.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
		|| (!(value.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && value.nNumberOfLinks != 1)) { return {}; }
	return QByteArray::number(value.dwVolumeSerialNumber) + ':'
		+ QByteArray::number(value.nFileIndexHigh) + ':' + QByteArray::number(value.nFileIndexLow);
#else
	struct stat value = {};
	if (::lstat(QFile::encodeName(path).constData(), &value) != 0) {
		return errno == ENOENT ? QByteArray("missing") : QByteArray();
	}
	if ((!S_ISDIR(value.st_mode) && !S_ISREG(value.st_mode))
		|| (S_ISREG(value.st_mode) && value.st_nlink != 1)) { return {}; }
	return QByteArray::number(qulonglong(value.st_dev)) + ':' + QByteArray::number(qulonglong(value.st_ino));
#endif
}

SyncStoreStatus ReadSyncPrivateFile(
		const QString &path,
		qsizetype limit,
		QByteArray &bytes) {
	const auto info = QFileInfo(path);
	if (!info.exists()) { return SyncStoreStatus::PendingMissing; }
	if (!info.isFile() || info.size() > limit) { return SyncStoreStatus::PendingMismatch; }
	if (!PrivatePermissions(path)) { return SyncStoreStatus::InsecurePermissions; }
	const auto identity = SyncPathIdentity(path);
	if (identity.isEmpty() || identity == "missing") { return SyncStoreStatus::IoError; }
	auto file = QFile(path);
#ifdef Q_OS_UNIX
	const auto descriptor = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (descriptor < 0) { return SyncStoreStatus::IoError; }
	struct stat opened = {};
	const auto same = ::fstat(descriptor, &opened) == 0 && S_ISREG(opened.st_mode)
		&& opened.st_nlink == 1 && opened.st_size <= limit
		&& QByteArray::number(qulonglong(opened.st_dev)) + ':' + QByteArray::number(qulonglong(opened.st_ino)) == identity;
	if (!same || !file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
		::close(descriptor);
		return SyncStoreStatus::IoError;
	}
#else
	const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (handle == INVALID_HANDLE_VALUE) { return SyncStoreStatus::IoError; }
	auto opened = BY_HANDLE_FILE_INFORMATION();
	const auto inspected = GetFileInformationByHandle(handle, &opened);
	const auto size = (quint64(opened.nFileSizeHigh) << 32) | opened.nFileSizeLow;
	const auto openedIdentity = QByteArray::number(opened.dwVolumeSerialNumber) + ':'
		+ QByteArray::number(opened.nFileIndexHigh) + ':' + QByteArray::number(opened.nFileIndexLow);
	auto count = DWORD();
	auto extra = char();
	auto extraCount = DWORD();
	auto ok = inspected && !(opened.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
		&& opened.nNumberOfLinks == 1 && openedIdentity == identity && size <= quint64(limit) && size == quint64(info.size());
	if (ok) {
		bytes.resize(qsizetype(size));
		ok = ReadFile(handle, bytes.data(), DWORD(size), &count, nullptr) && count == size
			&& ReadFile(handle, &extra, 1, &extraCount, nullptr) && extraCount == 0;
	}
	CloseHandle(handle);
	return ok && SyncPathIdentity(path) == identity && PrivatePermissions(path)
		? SyncStoreStatus::Ready : SyncStoreStatus::IoError;
#endif
	bytes = file.read(limit + 1);
	return file.error() == QFileDevice::NoError && file.atEnd() && bytes.size() == info.size()
		&& bytes.size() <= limit && SyncPathIdentity(path) == identity && PrivatePermissions(path)
		? SyncStoreStatus::Ready : SyncStoreStatus::IoError;
}

SyncStoreStatus WriteSyncPrivateFile(
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
	const auto status = ReadSyncPrivateFile(path, bytes.size(), readBack);
	return (status == SyncStoreStatus::Ready && readBack == bytes)
		? SyncStoreStatus::Ready
		: SyncStoreStatus::IoError;
}

bool SyncStoreWorkerThread() {
	const auto app = QCoreApplication::instance();
	return !app || QThread::currentThread() != app->thread();
}

SyncPrivateSnapshot CaptureSyncPrivateFile(const QString &path, qsizetype limit) {
	auto result = SyncPrivateSnapshot();
	const auto parent = QFileInfo(path).absolutePath();
	result.parentIdentity = SyncPathIdentity(parent);
	if (result.parentIdentity.isEmpty() || result.parentIdentity == "missing"
		|| !ReadablePrivateDirectory(QFileInfo(parent))) { return result; }
	result.identity = SyncPathIdentity(path);
	if (result.identity.isEmpty()) { return result; }
	if (result.identity == "missing") { result.status = SyncStoreStatus::Ready; return result; }
	result.status = ReadSyncPrivateFile(path, limit, result.bytes);
	if (result.status == SyncStoreStatus::Ready
		&& (SyncPathIdentity(path) != result.identity || SyncPathIdentity(parent) != result.parentIdentity)) {
		result.status = SyncStoreStatus::Changed;
	}
	return result;
}

namespace {

bool SameSnapshot(const QString &path, const SyncPrivateSnapshot &expected) {
	if (expected.status != SyncStoreStatus::Ready) { return false; }
	const auto fresh = CaptureSyncPrivateFile(path, std::max(qsizetype(1), expected.bytes.size()));
	return fresh.status == SyncStoreStatus::Ready && fresh.identity == expected.identity
		&& fresh.parentIdentity == expected.parentIdentity && fresh.bytes == expected.bytes;
}

bool FaultAllows(const SyncDurableFault &fault, SyncDurablePoint point) {
	return !fault || fault(point);
}

SyncStoreStatus FileBarrier(const QString &path, const SyncPrivateSnapshot &expected) {
	if (!SameSnapshot(path, expected) || expected.absent()) { return SyncStoreStatus::Changed; }
#ifdef Q_OS_WIN
	const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_WRITE,
		FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, nullptr);
	if (handle == INVALID_HANDLE_VALUE) { return SyncStoreStatus::DurabilityFailed; }
	auto info = BY_HANDLE_FILE_INFORMATION();
	const auto ok = GetFileInformationByHandle(handle, &info)
		&& !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
		&& info.nNumberOfLinks == 1
		&& QByteArray::number(info.dwVolumeSerialNumber) + ':' + QByteArray::number(info.nFileIndexHigh)
			+ ':' + QByteArray::number(info.nFileIndexLow) == expected.identity
		&& FlushFileBuffers(handle);
	const auto closed = CloseHandle(handle) != 0;
#else
	const auto fd = ::open(QFile::encodeName(path).constData(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) { return SyncStoreStatus::DurabilityFailed; }
	struct stat info = {};
	auto ok = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_nlink == 1
		&& info.st_uid == ::geteuid() && !(info.st_mode & 0077)
		&& QByteArray::number(qulonglong(info.st_dev)) + ':' + QByteArray::number(qulonglong(info.st_ino)) == expected.identity
		&& ::fsync(fd) == 0;
#ifdef Q_OS_MAC
	ok = ok && ::fcntl(fd, F_FULLFSYNC) == 0;
#endif
	const auto closed = ::close(fd) == 0;
#endif
	return ok && closed && SameSnapshot(path, expected)
		? SyncStoreStatus::Ready : SyncStoreStatus::DurabilityFailed;
}

}

SyncStoreStatus SyncPrivateDirectoryBarrier(const QString &path) {
	if (!SyncStoreWorkerThread()) { return SyncStoreStatus::WrongThread; }
	const auto identity = SyncPathIdentity(path);
	if (identity.isEmpty() || identity == "missing" || !ReadablePrivateDirectory(QFileInfo(path))) {
		return SyncStoreStatus::Changed;
	}
#ifdef Q_OS_WIN
	auto mount = QString(32768, QChar());
	if (!GetVolumePathNameW(reinterpret_cast<LPCWSTR>(path.utf16()), reinterpret_cast<LPWSTR>(mount.data()), DWORD(mount.size()))) {
		return SyncStoreStatus::DurabilityFailed;
	}
	auto volume = QString(32768, QChar());
	if (!GetVolumeNameForVolumeMountPointW(reinterpret_cast<LPCWSTR>(mount.utf16()), reinterpret_cast<LPWSTR>(volume.data()), DWORD(volume.size()))) {
		return SyncStoreStatus::DurabilityFailed;
	}
	volume.resize(volume.indexOf(QChar()));
	if (!volume.endsWith(u'\\')) { return SyncStoreStatus::DurabilityFailed; }
	volume.chop(1);
	const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(volume.utf16()), GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (handle == INVALID_HANDLE_VALUE) { return SyncStoreStatus::DurabilityFailed; }
	const auto ok = FlushFileBuffers(handle) != 0;
	const auto closed = CloseHandle(handle) != 0;
#else
	const auto fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) { return SyncStoreStatus::DurabilityFailed; }
	struct stat info = {};
	const auto ok = ::fstat(fd, &info) == 0 && S_ISDIR(info.st_mode)
		&& info.st_uid == ::geteuid() && !(info.st_mode & 0077)
		&& QByteArray::number(qulonglong(info.st_dev)) + ':' + QByteArray::number(qulonglong(info.st_ino)) == identity
		&& ::fsync(fd) == 0;
	const auto closed = ::close(fd) == 0;
#endif
	return ok && closed && SyncPathIdentity(path) == identity
		? SyncStoreStatus::Ready : SyncStoreStatus::DurabilityFailed;
}

SyncStoreStatus ConfirmSyncDurableFile(
		const QString &path, const SyncPrivateSnapshot &expected, const SyncDurableFault &fault) {
	if (!SyncStoreWorkerThread()) { return SyncStoreStatus::WrongThread; }
	if (!FaultAllows(fault, SyncDurablePoint::FileBarrier)) { return SyncStoreStatus::DurabilityFailed; }
	const auto file = FileBarrier(path, expected);
	if (file != SyncStoreStatus::Ready) { return file; }
	if (!FaultAllows(fault, SyncDurablePoint::DirectoryBarrier)) { return SyncStoreStatus::DurabilityFailed; }
	const auto directory = SyncPrivateDirectoryBarrier(QFileInfo(path).absolutePath());
	return directory == SyncStoreStatus::Ready ? FileBarrier(path, expected) : directory;
}

SyncStoreStatus WriteSyncDurableFile(
		const QString &path, const QByteArray &bytes,
		const SyncPrivateSnapshot &expected, const SyncDurableFault &fault) {
	if (!SyncStoreWorkerThread()) { return SyncStoreStatus::WrongThread; }
	if (!SameSnapshot(path, expected)) { return SyncStoreStatus::Changed; }
	if (!FaultAllows(fault, SyncDurablePoint::BeforeWrite)) { return SyncStoreStatus::DurabilityFailed; }
	auto file = QSaveFile(path); file.setDirectWriteFallback(false);
	if (!file.open(QIODevice::WriteOnly)) { return SyncStoreStatus::IoError; }
#ifdef Q_OS_UNIX
	if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) { return SyncStoreStatus::InsecurePermissions; }
#endif
	const auto half = bytes.size() / 2;
	if (file.write(bytes.constData(), half) != half || !FaultAllows(fault, SyncDurablePoint::PartialWrite)
		|| file.write(bytes.constData() + half, bytes.size() - half) != bytes.size() - half
		|| !file.flush()) { return SyncStoreStatus::DurabilityFailed; }
	if (!FaultAllows(fault, SyncDurablePoint::BeforeRename) || !SameSnapshot(path, expected)) {
		return SyncStoreStatus::Changed;
	}
	if (!file.commit()) { return SyncStoreStatus::IoError; }
	if (!FaultAllows(fault, SyncDurablePoint::AfterRename)) { return SyncStoreStatus::DurabilityFailed; }
	const auto actual = CaptureSyncPrivateFile(path, std::max(qsizetype(1), bytes.size()));
	if (actual.status != SyncStoreStatus::Ready || actual.bytes != bytes
		|| actual.parentIdentity != expected.parentIdentity) { return SyncStoreStatus::Changed; }
	return ConfirmSyncDurableFile(path, actual, fault);
}

SyncStoreStatus SyncLocalStore::CheckpointRetainedConfigBases(const SyncDurableFault &fault) {
	if (!SyncStoreWorkerThread()) { return SyncStoreStatus::WrongThread; }
	if (_carryOwner || _status != SyncStoreStatus::Ready || !_state || !_lock || !_lock->isLocked()) {
		return SyncStoreStatus::InvalidTransition;
	}
	auto next = *_state;
	if (!_retainedBootstrap.isEmpty()) {
		const auto status = RetainSyncConfigRecord(next, _retainedBootstrap);
		if (status != SyncStoreStatus::Ready) { return status; }
	}
	const auto retained = RetainedOf(next);
	if (retained.status != SyncStoreStatus::Ready) { return retained.status; }
	if (retained.entries.isEmpty() && next.configData.base.isEmpty()) { return SyncStoreStatus::Ready; }
	const auto base = ReadSyncRetainedConfigBase(next);
	if (!next.configData.base.isEmpty() && !base) { return SyncStoreStatus::MissingLocalVersion; }
	const auto expected = CaptureSyncPrivateFile(StatePath(), kStateBytes);
	if (expected.status != SyncStoreStatus::Ready || expected.absent()
		|| SerializeSyncLocalState(ParseSyncLocalState(expected.bytes).state).canonical != SerializeSyncLocalState(*_state).canonical) {
		return SyncStoreStatus::Changed;
	}
	const auto directory = _root + u"/retained-bases"_q;
	const auto created = EnsureSyncPrivateDirectory(directory);
	if (created != SyncStoreStatus::Ready) { return created; }
	if (SyncPrivateDirectoryBarrier(_root) != SyncStoreStatus::Ready) { return SyncStoreStatus::DurabilityFailed; }
	auto kept = QJsonArray();
	for (const auto &entry : retained.entries) {
		const auto object = entry.toObject();
		const auto record = object.value(u"record"_q).toString().toUtf8();
		const auto path = directory + u'/' + object.value(u"sha256"_q).toString() + u".json"_q;
		const auto old = CaptureSyncPrivateFile(path, kConfigBytes);
		if (old.status != SyncStoreStatus::Ready || (!old.absent() && old.bytes != record)) { return SyncStoreStatus::Changed; }
		const auto status = old.absent() ? WriteSyncDurableFile(path, record, old, fault)
			: ConfirmSyncDurableFile(path, old, fault);
		if (status != SyncStoreStatus::Ready) { return status; }
		if (record == base.staged || object.size() != 2) { kept.append(entry); }
	}
	auto object = retained.object; object.insert(u"records"_q, kept);
	next.preserved.insert(QString::fromUtf16(kRetainedField), object);
	const auto encoded = SerializeSyncLocalState(next);
	if (!encoded) { return SyncStoreStatus::RetentionFull; }
	const auto status = encoded.canonical == expected.bytes
		? ConfirmSyncDurableFile(StatePath(), expected, fault)
		: WriteSyncDurableFile(StatePath(), encoded.canonical, expected, fault);
	if (status != SyncStoreStatus::Ready) { _status = status; return status; }
	_state = std::move(next);
	_retainedBootstrap.clear();
	return SyncStoreStatus::Ready;
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

SyncRecoveryRead ReadSyncRecoveryStore(
		const AccountFiles &files, std::optional<AccountKey> directoryAccount) {
	const auto read = [&] {
		auto result = SyncRecoveryRead();
		result.store.directoryAccount = directoryAccount;
		const auto capture = [&](const QString &path) {
			const auto identity = SyncPathIdentity(path);
			result.identities[path] = identity;
			return !identity.isEmpty();
		};
		const auto root = QFileInfo(files.syncDirectory);
		if (!capture(files.syncDirectory) || (root.exists() && !ReadablePrivateDirectory(root))) { return result; }
		const auto state = QFileInfo(files.syncStore);
		result.store.statePresent = state.exists() || state.isSymLink();
		if (!capture(files.syncStore)) { return result; }
		if (result.store.statePresent) {
			const auto status = ReadSyncPrivateFile(files.syncStore, kStateBytes, result.store.state);
			if (status != SyncStoreStatus::Ready) { result.status = status; return result; }
		}
		const auto pendingPath = files.syncDirectory + u"/pending"_q;
		const auto pending = QFileInfo(pendingPath);
		result.store.pendingPresent = pending.exists() || pending.isSymLink();
		if (!capture(pendingPath) || (pending.exists() && !ReadablePrivateDirectory(pending))) { return result; }
		if (pending.exists()) {
			auto entries = QDirIterator(pendingPath,
				QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
			auto total = qsizetype(0);
			while (entries.hasNext()) {
				const auto path = entries.next();
				auto bytes = QByteArray();
				const auto status = ReadSyncPrivateFile(path, kStateBytes, bytes);
				if (status != SyncStoreStatus::Ready || !capture(path)
					|| result.store.pendingFiles.size() >= 1024) { return result; }
				total += bytes.size();
				if (total > 16 * 1024 * 1024) { return result; }
				result.store.pendingFiles[entries.fileName()] = bytes;
			}
		}
		for (const auto &[path, identity] : result.identities) {
			if (SyncPathIdentity(path) != identity) { return SyncRecoveryRead(); }
		}
		result.store.pendingComplete = true;
		result.status = SyncStoreStatus::Ready;
		return result;
	};
	const auto first = read();
	if (first.status != SyncStoreStatus::Ready) { return first; }
	const auto second = read();
	if (second.status != SyncStoreStatus::Ready || first.identities != second.identities
		|| first.store.statePresent != second.store.statePresent || first.store.state != second.store.state
		|| first.store.pendingPresent != second.store.pendingPresent || first.store.pendingFiles != second.store.pendingFiles) {
		return {};
	}
	return second;
}

SyncStoreResult SyncLocalStore::Open(bool optedIn, const QByteArray &bindingToken) {
	return OpenChecked(optedIn, bindingToken, false);
}

SyncStoreResult SyncLocalStore::OpenForCarry(const QByteArray &bindingToken) {
	return OpenChecked(true, bindingToken, true);
}

SyncStoreResult SyncLocalStore::OpenChecked(
		bool optedIn, const QByteArray &bindingToken, bool carryOwner) {
	if (_opened) {
		return { SyncStoreStatus::InvalidTransition };
	}
	if (!optedIn) {
		return { SyncStoreStatus::Disabled };
	}
	_opened = true;
	_carryOwner = carryOwner;
	if (_root.isEmpty()
		|| !QDir().mkpath(QFileInfo(_root).absolutePath())) {
		return SetFailure({ SyncStoreStatus::IoError });
	}
	const auto rootStatus = EnsureSyncPrivateDirectory(_root);
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
	const auto carryPath = _root + u"/carry.json"_q;
	if (!carryOwner && SyncPathIdentity(carryPath) != "missing") {
		return SetFailure({ SyncStoreStatus::CarryPending });
	}
	if (QFileInfo::exists(_root + u"/recovery.json"_q) || QFileInfo(_root + u"/recovery.json"_q).isSymLink()) {
		return SetFailure({ SyncStoreStatus::InvalidTransition });
	}
	if (bindingToken.isEmpty()) {
		const auto pendingStatus = EnsureSyncPrivateDirectory(_root + u"/pending"_q);
		if (pendingStatus != SyncStoreStatus::Ready) { return SetFailure({ pendingStatus }); }
	}
	const auto stateInfo = QFileInfo(StatePath());
	if (!stateInfo.exists() && !stateInfo.isSymLink()) {
		if (!bindingToken.isEmpty() && !carryOwner) { return SetFailure({ SyncStoreStatus::AccountUnbound }); }
		if (carryOwner) { return SetFailure({ SyncStoreStatus::Uninitialized }); }
		const auto stageStatus = CheckStages(
			_root + u"/pending"_q, nullptr);
		return SetFailure({ stageStatus == SyncStoreStatus::Ready
			? SyncStoreStatus::Uninitialized : stageStatus });
	}
	auto bytes = QByteArray();
	const auto readStatus = ReadSyncPrivateFile(
		StatePath(),
		kStateBytes,
		bytes);
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
	if (!bindingToken.isEmpty()) {
		if (CheckSyncAccountBinding(parsed.state, bindingToken) != SyncAccountBindingVerdict::Bound) {
			return SetFailure({ SyncStoreStatus::AccountUnbound });
		}
		const auto pendingStatus = EnsureSyncPrivateDirectory(_root + u"/pending"_q);
		if (pendingStatus != SyncStoreStatus::Ready) { return SetFailure({ pendingStatus }); }
	}
	const auto retention = ValidateSyncRetainedConfig(parsed.state);
	if (retention != SyncStoreStatus::Ready) { return SetFailure({ retention }); }
	_state = parsed.state;
	_status = SyncStoreStatus::Ready;
	if (carryOwner) { return { SyncStoreStatus::Ready }; }
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
	const auto retention = ValidateSyncRetainedConfig(initial);
	if (retention != SyncStoreStatus::Ready) { return { retention }; }
	const auto serialized = SerializeSyncLocalState(initial);
	if (!serialized) {
		return {
			SyncStoreStatus::InvalidState,
			serialized.error,
		};
	}
	const auto writeStatus = WriteSyncPrivateFile(
		StatePath(),
		serialized.canonical);
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
	const auto readStatus = ReadSyncPrivateFile(
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
		const SyncLocalConfigState &nextConfigData,
		const QByteArray &accountBindingToken) {
	if (_status != SyncStoreStatus::Ready || !_state || _carryOwner) {
		return { SyncStoreStatus::InvalidTransition };
	}
	if (CheckSyncAccountBinding(*_state, accountBindingToken)
		!= SyncAccountBindingVerdict::Bound) {
		return { SyncStoreStatus::AccountUnbound };
	}
	if (canonicalRecord.size() > kConfigBytes) { return { SyncStoreStatus::BaseRecordTooLarge }; }
	auto reserved = ReserveConfigRecord(*_state, canonicalRecord);
	if (!reserved.result) {
		return reserved.result;
	}
	if (nextConfigData.pending != reserved.key) {
		return { SyncStoreStatus::InvalidRecord };
	}
	auto next = std::move(reserved.state);
	next.configData = nextConfigData;
	const auto retention = PrepareRetainedConfig(next, canonicalRecord);
	if (retention != SyncStoreStatus::Ready) { return { retention }; }
	const auto serialized = SerializeSyncLocalState(next);
	if (!serialized) {
		return { SyncStoreStatus::InvalidState, serialized.error };
	}
	const auto path = PendingPath(next.config.pendingSeq);
	const auto stageInfo = QFileInfo(path);
	if (stageInfo.exists() || stageInfo.isSymLink()) {
		return SetFailure({ SyncStoreStatus::OrphanStage });
	}
	const auto stageStatus = WriteSyncPrivateFile(path, canonicalRecord);
	if (stageStatus != SyncStoreStatus::Ready) {
		return SetFailure({ stageStatus });
	}
	const auto stateStatus = WriteSyncPrivateFile(
		StatePath(),
		serialized.canonical);
	if (stateStatus != SyncStoreStatus::Ready) {
		const auto old = SerializeSyncLocalState(*_state);
		auto current = QByteArray();
		if (old
			&& ReadSyncPrivateFile(StatePath(), kStateBytes, current)
				== SyncStoreStatus::Ready
			&& current == old.canonical) {
			QFile::remove(path);
		}
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
		int32_t messageId,
		PeerToken chat) {
	if (_status != SyncStoreStatus::Ready || !_state || _carryOwner) {
		return { SyncStoreStatus::InvalidTransition };
	}
	if (!DesktopSyncChatLedgerMatches(*_state, chat)) {
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
	auto nextState = recorded.state;
	const auto retention = PrepareRetainedConfig(nextState, serverRecord);
	if (retention != SyncStoreStatus::Ready) { return { retention }; }
	const auto serialized = SerializeSyncLocalState(nextState);
	if (!serialized) {
		return { SyncStoreStatus::InvalidState, serialized.error };
	}
	const auto stateStatus = WriteSyncPrivateFile(
		StatePath(),
		serialized.canonical);
	if (stateStatus != SyncStoreStatus::Ready) {
		return SetFailure({ stateStatus });
	}
	_state = std::move(nextState);
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

SyncStoreResult SyncLocalStore::CommitConfigData(
		const SyncLocalConfigState &next,
		const QByteArray &accountBindingToken,
		const QByteArray &baseRecord) {
	if (_status != SyncStoreStatus::Ready || !_state || _carryOwner) {
		return { SyncStoreStatus::InvalidTransition };
	}
	if (CheckSyncAccountBinding(*_state, accountBindingToken)
		!= SyncAccountBindingVerdict::Bound) {
		return { SyncStoreStatus::AccountUnbound };
	}
	const auto checked = CheckSyncConfigDataCommit(*_state, next);
	switch (checked.status) {
	case SyncConfigCommitStatus::Ready:
		break;
	case SyncConfigCommitStatus::Unchanged:
		return { SyncStoreStatus::Ready };
	case SyncConfigCommitStatus::InvalidTransition:
		return { SyncStoreStatus::InvalidTransition };
	case SyncConfigCommitStatus::InvalidState:
		return { SyncStoreStatus::InvalidState, checked.error };
	}
	auto retainedState = checked.state;
	const auto retention = PrepareRetainedConfig(retainedState, baseRecord);
	if (retention != SyncStoreStatus::Ready) { return { retention }; }
	const auto serialized = SerializeSyncLocalState(retainedState);
	if (!serialized) { return { SyncStoreStatus::RetentionFull }; }
	const auto stateStatus = WriteSyncPrivateFile(
		StatePath(),
		serialized.canonical);
	if (stateStatus != SyncStoreStatus::Ready) {
		return SetFailure({ stateStatus });
	}
	_state = std::move(retainedState);
	return { SyncStoreStatus::Ready };
}

SyncStoreResult SyncLocalStore::RemoveAbsentConfigMessage(
		int32_t messageId,
		const QByteArray &record,
		const QByteArray &accountBindingToken,
		PeerToken chat) {
	if (_status != SyncStoreStatus::Ready || !_state
		|| _carryOwner || !DesktopSyncChatLedgerMatches(*_state, chat)
		|| CheckSyncAccountBinding(*_state, accountBindingToken) != SyncAccountBindingVerdict::Bound
		|| !CheckOwnConfigMessageDeletion(*_state, messageId, record)) {
		return { SyncStoreStatus::InvalidTransition };
	}
	const auto removed = RemoveAbsentOwnConfigMessage(*_state, messageId, SyncOwnMessagePresence::Absent);
	if (!removed) { return { SyncStoreStatus::InvalidTransition }; }
	const auto next = SerializeSyncLocalState(removed.state);
	if (!next) { return { SyncStoreStatus::InvalidState }; }
	const auto status = WriteSyncPrivateFile(StatePath(), next.canonical);
	if (status != SyncStoreStatus::Ready) { return SetFailure({ status }); }
	_state = removed.state;
	return { SyncStoreStatus::Ready };
}

const SyncLocalState *SyncLocalStore::state() const {
	return _status == SyncStoreStatus::Ready && _state
		? &*_state : nullptr;
}

const QString &SyncLocalStore::directory() const { return _root; }

SyncStoreStatus SyncLocalStore::status() const {
	return _status;
}

}

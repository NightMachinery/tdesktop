/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_config_history.h"

#include "base/random.h"
#include "purple/purple_config.h"
#include "purple/purple_config_sync.h"
#include "purple/purple_state.h"
#include "purple/purple_sync_local_store.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSet>

#include <algorithm>

namespace Purple {
namespace {

constexpr auto kHistoryLimit = 30;
constexpr auto kMaximumTextBytes = 256 * 1024;
constexpr auto kMaximumMetadataBytes = 16 * 1024;
constexpr auto kMaximumLabelLength = 256;
constexpr auto kMetadataVersion = 1;
constexpr auto kTimeDigits = 16;
constexpr auto kSuffixDigits = 16;
constexpr auto kIdLength = kTimeDigits + 1 + kSuffixDigits;
constexpr auto kIdAttempts = 4;
constexpr auto kMaximumSafeInteger = int64(9007199254740991LL);

struct LoadedEntry {
	SyncConfigHistoryEntry entry;
	QByteArray text;
};

[[nodiscard]] QString ReasonName(SyncConfigHistoryReason reason) {
	switch (reason) {
	case SyncConfigHistoryReason::BeforeUpdate: return u"before_update"_q;
	case SyncConfigHistoryReason::BeforeChoice: return u"before_choice"_q;
	case SyncConfigHistoryReason::BeforeRestore: return u"before_restore"_q;
	case SyncConfigHistoryReason::BeforeUndo: return u"before_undo"_q;
	}
	return QString();
}

[[nodiscard]] std::optional<SyncConfigHistoryReason> ParseReason(
		const QString &name) {
	for (const auto reason : {
		SyncConfigHistoryReason::BeforeUpdate,
		SyncConfigHistoryReason::BeforeChoice,
		SyncConfigHistoryReason::BeforeRestore,
		SyncConfigHistoryReason::BeforeUndo,
	}) {
		if (ReasonName(reason) == name) {
			return reason;
		}
	}
	return std::nullopt;
}

[[nodiscard]] bool IsHistoryId(const QString &id) {
	if (id.size() != kIdLength || id[kTimeDigits].unicode() != u'-') {
		return false;
	}
	for (auto i = 0; i != kIdLength; ++i) {
		if (i == kTimeDigits) {
			continue;
		}
		const auto code = id[i].unicode();
		const auto digit = (code >= u'0' && code <= u'9');
		const auto hex = (i > kTimeDigits && code >= u'a' && code <= u'f');
		if (!digit && !hex) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] int64 IdTime(const QString &id) {
	return id.left(kTimeDigits).toLongLong();
}

[[nodiscard]] QString NameId(const QString &name) {
	if (name.size() <= kIdLength || name[kIdLength].unicode() != u'.') {
		return QString();
	}
	const auto id = name.left(kIdLength);
	return IsHistoryId(id) ? id : QString();
}

[[nodiscard]] QString TextPath(const QString &directory, const QString &id) {
	return directory + u"/"_q + id + u".toml"_q;
}

[[nodiscard]] QString MetadataPath(
		const QString &directory,
		const QString &id) {
	return directory + u"/"_q + id + u".json"_q;
}

[[nodiscard]] QStringList EntryNames(const QString &directory) {
	return QDir(directory).entryList(
		QDir::Files | QDir::Hidden | QDir::System,
		QDir::Name);
}

[[nodiscard]] std::vector<QString> EntryIds(const QStringList &names) {
	auto result = std::vector<QString>();
	for (const auto &name : names) {
		const auto id = NameId(name);
		if (!id.isEmpty()
			&& std::find(result.begin(), result.end(), id) == result.end()) {
			result.push_back(id);
		}
	}
	return result;
}

void SortNewestFirst(std::vector<SyncConfigHistoryEntry> &entries) {
	std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
		return a.id > b.id;
	});
}

[[nodiscard]] bool CheckDirectories() {
	for (const auto &path : {
		ConfigDirectory() + u"/sync"_q,
		SyncConfigHistoryDirectory(),
	}) {
		const auto status = EnsureSyncPrivateDirectory(path);
		if (status != SyncStoreStatus::Ready) {
			LOG(("Purple Error: Settings history refused %1, status %2."
				).arg(path
				).arg(int(status)));
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool PrepareDirectories() {
	if (!QDir().mkpath(ConfigDirectory())) {
		LOG(("Purple Error: Could not create %1.").arg(ConfigDirectory()));
		return false;
	}
	return CheckDirectories();
}

[[nodiscard]] bool ExistingDirectories() {
	const auto info = QFileInfo(SyncConfigHistoryDirectory());
	return (info.exists() || info.isSymLink()) && CheckDirectories();
}

[[nodiscard]] QByteArray SerializeMetadata(
		const SyncConfigHistoryEntry &entry) {
	auto object = QJsonObject();
	object.insert(u"version"_q, kMetadataVersion);
	object.insert(u"id"_q, entry.id);
	object.insert(u"created_ms"_q, QJsonValue(qint64(entry.createdMs)));
	object.insert(u"reason"_q, ReasonName(entry.reason));
	object.insert(u"label"_q, entry.label);
	object.insert(u"version_key"_q, entry.versionKey);
	object.insert(u"fingerprint"_q, entry.fingerprint);
	object.insert(u"size"_q, QJsonValue(qint64(entry.size)));
	object.insert(u"existed"_q, entry.existed);
	return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

[[nodiscard]] std::optional<LoadedEntry> LoadEntry(
		const QString &directory,
		const QString &id) {
	if (!IsHistoryId(id)) {
		return std::nullopt;
	}
	auto metadata = QByteArray();
	const auto metadataStatus = ReadSyncPrivateFile(
		MetadataPath(directory, id),
		kMaximumMetadataBytes,
		metadata);
	if (metadataStatus != SyncStoreStatus::Ready) {
		return std::nullopt;
	}
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(metadata, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		return std::nullopt;
	}
	const auto object = document.object();
	const auto version = object.value(u"version"_q);
	const auto storedId = object.value(u"id"_q);
	const auto created = object.value(u"created_ms"_q);
	const auto reason = ParseReason(object.value(u"reason"_q).toString());
	const auto label = object.value(u"label"_q);
	const auto versionKey = object.value(u"version_key"_q);
	const auto fingerprint = object.value(u"fingerprint"_q);
	const auto size = object.value(u"size"_q);
	const auto existed = object.value(u"existed"_q);
	if (!version.isDouble()
		|| version.toInteger(-1) != kMetadataVersion
		|| storedId.toString() != id
		|| !created.isDouble()
		|| created.toInteger(-1) != IdTime(id)
		|| IdTime(id) <= 0
		|| IdTime(id) > kMaximumSafeInteger
		|| !reason
		|| !label.isString()
		|| label.toString().size() > kMaximumLabelLength
		|| !versionKey.isString()
		|| (!versionKey.toString().isEmpty()
			&& !IsConfigVersionKey(versionKey.toString()))
		|| !fingerprint.isString()
		|| !size.isDouble()
		|| size.toInteger(-1) < 0
		|| size.toInteger(-1) > kMaximumTextBytes
		|| !existed.isBool()
		|| (!existed.toBool() && size.toInteger(-1) != 0)) {
		return std::nullopt;
	}
	auto text = QByteArray();
	const auto textStatus = ReadSyncPrivateFile(
		TextPath(directory, id),
		kMaximumTextBytes,
		text);
	if (textStatus != SyncStoreStatus::Ready
		|| text.size() != size.toInteger(-1)
		|| SettingsFingerprint(text) != fingerprint.toString()) {
		return std::nullopt;
	}
	return LoadedEntry{
		.entry = {
			.id = id,
			.createdMs = IdTime(id),
			.reason = *reason,
			.label = label.toString(),
			.versionKey = versionKey.toString(),
			.fingerprint = fingerprint.toString(),
			.size = text.size(),
			.existed = existed.toBool(),
		},
		.text = text,
	};
}

[[nodiscard]] QString UnusedId(const QString &directory, int64 createdMs) {
	for (auto attempt = 0; attempt != kIdAttempts; ++attempt) {
		const auto suffix = base::RandomValue<uint64_t>();
		const auto id = u"%1-%2"_q
			.arg(createdMs, kTimeDigits, 10, QChar(u'0'))
			.arg(suffix, kSuffixDigits, 16, QChar(u'0'));
		const auto text = QFileInfo(TextPath(directory, id));
		const auto metadata = QFileInfo(MetadataPath(directory, id));
		if (!text.exists() && !text.isSymLink()
			&& !metadata.exists() && !metadata.isSymLink()) {
			return id;
		}
	}
	return QString();
}

void Discard(const QString &directory, const QString &id) {
	for (const auto &path : {
		TextPath(directory, id),
		MetadataPath(directory, id),
	}) {
		const auto info = QFileInfo(path);
		if ((info.exists() || info.isSymLink()) && !QFile::remove(path)) {
			LOG(("Purple Error: Could not remove %1.").arg(path));
		}
	}
}

void Prune(const QString &directory, const QSet<QString> &keep) {
	const auto names = EntryNames(directory);
	auto valid = std::vector<SyncConfigHistoryEntry>();
	for (const auto &id : EntryIds(names)) {
		if (const auto loaded = LoadEntry(directory, id)) {
			valid.push_back(loaded->entry);
		}
	}
	SortNewestFirst(valid);
	auto validIds = QSet<QString>();
	for (const auto &entry : valid) {
		validIds.insert(entry.id);
	}
	auto kept = QSet<QString>();
	for (const auto &id : keep) {
		if (validIds.contains(id)) {
			kept.insert(id);
		}
	}
	for (const auto &entry : valid) {
		if (kept.size() < kHistoryLimit) {
			kept.insert(entry.id);
		}
	}
	const auto full = (kept.size() >= kHistoryLimit);
	const auto oldestKept = full
		? *std::min_element(kept.begin(), kept.end())
		: QString();
	for (const auto &name : names) {
		const auto id = NameId(name);
		if (id.isEmpty() || kept.contains(id)) {
			continue;
		}
		const auto obsolete = validIds.contains(id)
			|| (full && id < oldestKept);
		const auto path = directory + u"/"_q + name;
		if (obsolete && !QFile::remove(path)) {
			LOG(("Purple Error: Could not remove %1.").arg(path));
		}
	}
}

} // namespace

QString SyncConfigHistoryDirectory() {
	return ConfigDirectory() + u"/sync/history"_q;
}

std::optional<SyncConfigHistoryEntry> SaveSyncConfigHistory(
		const std::optional<QByteArray> &text,
		SyncConfigHistoryReason reason,
		const QString &label,
		const QString &versionKey,
		const QString &keepId) {
	const auto bytes = text.value_or(QByteArray());
	if (bytes.size() > kMaximumTextBytes
		|| ReasonName(reason).isEmpty()
		|| label.size() > kMaximumLabelLength
		|| (!versionKey.isEmpty() && !IsConfigVersionKey(versionKey))) {
		LOG(("Purple Error: Refused an invalid settings history entry."));
		return std::nullopt;
	}
	const auto createdMs = int64(QDateTime::currentMSecsSinceEpoch());
	if (createdMs <= 0 || createdMs > kMaximumSafeInteger) {
		LOG(("Purple Error: Settings history has no usable clock."));
		return std::nullopt;
	}
	if (!PrepareDirectories()) {
		return std::nullopt;
	}
	const auto directory = SyncConfigHistoryDirectory();
	const auto id = UnusedId(directory, createdMs);
	if (id.isEmpty()) {
		LOG(("Purple Error: Could not choose a settings history id."));
		return std::nullopt;
	}
	const auto entry = SyncConfigHistoryEntry{
		.id = id,
		.createdMs = createdMs,
		.reason = reason,
		.label = label,
		.versionKey = versionKey,
		.fingerprint = SettingsFingerprint(bytes),
		.size = bytes.size(),
		.existed = text.has_value(),
	};
	const auto textStatus = WriteSyncPrivateFile(
		TextPath(directory, id),
		bytes);
	const auto metadataStatus = (textStatus == SyncStoreStatus::Ready)
		? WriteSyncPrivateFile(
			MetadataPath(directory, id),
			SerializeMetadata(entry))
		: textStatus;
	const auto saved = (metadataStatus == SyncStoreStatus::Ready)
		? LoadEntry(directory, id)
		: std::nullopt;
	if (!saved || saved->text != bytes) {
		LOG(("Purple Error: Could not save settings history %1, status %2."
			).arg(id
			).arg(int(metadataStatus)));
		Discard(directory, id);
		return std::nullopt;
	}
	auto keep = QSet<QString>{ id };
	if (!keepId.isEmpty()) {
		keep.insert(keepId);
	}
	Prune(directory, keep);
	return saved->entry;
}

std::vector<SyncConfigHistoryEntry> ListSyncConfigHistory() {
	if (!ExistingDirectories()) {
		return {};
	}
	const auto directory = SyncConfigHistoryDirectory();
	auto result = std::vector<SyncConfigHistoryEntry>();
	auto invalid = 0;
	for (const auto &id : EntryIds(EntryNames(directory))) {
		if (const auto loaded = LoadEntry(directory, id)) {
			result.push_back(loaded->entry);
		} else {
			++invalid;
		}
	}
	if (invalid) {
		LOG(("Purple Error: Skipped %1 invalid settings history entries."
			).arg(invalid));
	}
	SortNewestFirst(result);
	return result;
}

std::optional<QByteArray> ReadSyncConfigHistory(const QString &id) {
	if (!IsHistoryId(id) || !ExistingDirectories()) {
		LOG(("Purple Error: No settings history entry %1.").arg(id));
		return std::nullopt;
	}
	const auto loaded = LoadEntry(SyncConfigHistoryDirectory(), id);
	if (!loaded) {
		LOG(("Purple Error: Settings history entry %1 is invalid.").arg(id));
		return std::nullopt;
	}
	return loaded->text;
}

} // namespace Purple

#include "purple/purple_archive_journal.h"
#include "purple/purple_archive_attention.h"
#include "purple/purple_archive_files.h"
#include "purple/purple_sync_json.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <algorithm>
#include <limits>

namespace Purple {
namespace {

std::optional<uint64_t> Number(const QJsonValue &value) {
	if (!value.isString()) {
		return {};
	}
	auto ok = false;
	const auto result = value.toString().toULongLong(&ok);
	return ok && QString::number(result) == value.toString()
			   ? std::optional<uint64_t>(result)
			   : std::nullopt;
}

QString Token(const DesktopArchiveJournalAccess &access, const QString &id) {
	return ArchiveSqlToken(access.epoch.account, ArchiveIndexKind::Event,
		(u"outbox/"_q + id).toUtf8(), access.security);
}

std::optional<QByteArray> Encode(const ArchiveEvent &event) {
	if (!IsArchiveMessageKey(event.key) || !IsArchiveToken(event.id) || !event.order
		|| !event.migrationAliases.empty()) {
		return {};
	}
	auto version = ArchiveVersion();
	version.id = event.id;
	version.revision = !event.missing.revision.isEmpty()		  ? event.missing.revision
					   : event.origin == ArchiveOrigin::Hydration ? event.targetRevision
																  : event.id;
	version.order = event.order;
	version.firstObservedAt = event.observation.observedAt;
	version.observation = event.observation;
	version.content = event.content.value_or(ArchiveContent());
	version.completeness
		= event.content ? event.completeness : ArchiveCompleteness::Complete;
	version.missing = event.missing;
	version.missing.revision = version.revision;
	version.source = event.source;
	for (const auto &descriptor : version.content.media) {
		version.media.push_back({descriptor.identity});
	}
	const auto snapshot = EncodeArchiveVersionSnapshot({event.key, version});
	if (!snapshot) {
		return {};
	}
	auto object = QJsonObject{{u"format"_q, u"desktop-archive-capture"_q},
		{u"version"_q, 1}, {u"snapshot"_q, QJsonDocument::fromJson(*snapshot).object()},
		{u"content"_q, event.content.has_value()}, {u"origin"_q, int(event.origin)},
		{u"accountGeneration"_q, QString::number(event.generation.account)},
		{u"chatGeneration"_q, QString::number(event.generation.chat)},
		{u"target"_q, event.targetVersion}, {u"revision"_q, event.targetRevision},
		{u"hash"_q, event.expectedContentHash}};
	if (event.lifecycleEpoch) {
		const auto &epoch = *event.lifecycleEpoch;
		const auto account
			= AccountKeyText(epoch.account.environment, epoch.account.userId);
		if (!account || !IsArchiveToken(epoch.storeId) || !epoch.generation
			|| epoch.removalPending || !IsArchiveToken(event.lifecycleMediaIdentity)) {
			return {};
		}
		object.insert(u"lifecycle"_q,
			QJsonObject{{u"account"_q, *account}, {u"store"_q, epoch.storeId},
				{u"generation"_q, QString::number(epoch.generation)},
				{u"media"_q, event.lifecycleMediaIdentity}});
	} else if (!event.lifecycleMediaIdentity.isEmpty()) {
		return {};
	}
	const auto bytes
		= CanonicalizeSyncJson(QJsonDocument(object).toJson(QJsonDocument::Compact));
	return bytes ? std::optional(bytes.canonical) : std::nullopt;
}

std::optional<ArchiveEvent> Decode(const QByteArray &bytes) {
	const auto canonical = CanonicalizeSyncJson(bytes);
	if (!canonical || canonical.canonical != bytes) {
		return {};
	}
	const auto object = QJsonDocument::fromJson(bytes).object();
	const auto account = Number(object.value(u"accountGeneration"_q));
	const auto chat = Number(object.value(u"chatGeneration"_q));
	const auto snapshot = DecodeArchiveVersionSnapshot(
		QJsonDocument(object.value(u"snapshot"_q).toObject())
			.toJson(QJsonDocument::Compact));
	if (object.size() != (object.contains(u"lifecycle"_q) ? 11 : 10)
		|| object.value(u"format"_q) != QJsonValue(u"desktop-archive-capture"_q)
		|| object.value(u"version"_q) != QJsonValue(1)
		|| !object.value(u"content"_q).isBool() || !object.value(u"origin"_q).isDouble()
		|| object.value(u"origin"_q).toInt(-1) < 0
		|| object.value(u"origin"_q).toInt() > int(ArchiveOrigin::SendMetadata)
		|| !object.value(u"target"_q).isString()
		|| !object.value(u"revision"_q).isString() || !object.value(u"hash"_q).isString()
		|| !account || !chat || !snapshot.snapshot) {
		return {};
	}
	const auto &version = snapshot.snapshot->version;
	auto event = ArchiveEvent();
	event.key = snapshot.snapshot->key;
	event.id = version.id;
	event.order = version.order;
	event.origin = ArchiveOrigin(object.value(u"origin"_q).toInt());
	event.generation = {*account, *chat};
	if (object.value(u"content"_q).toBool()) {
		event.content = version.content;
	}
	event.completeness = version.completeness;
	event.missing = version.missing;
	event.observation = version.observation;
	event.source = version.source;
	event.targetVersion = object.value(u"target"_q).toString();
	event.targetRevision = object.value(u"revision"_q).toString();
	event.expectedContentHash = object.value(u"hash"_q).toString();
	if (object.contains(u"lifecycle"_q)) {
		const auto lifecycle = object.value(u"lifecycle"_q).toObject();
		const auto account = ParseAccountKey(lifecycle.value(u"account"_q).toString());
		const auto generation = Number(lifecycle.value(u"generation"_q));
		if (lifecycle.size() != 4 || !account || !generation) {
			return {};
		}
		event.lifecycleEpoch = ArchiveStoreEpoch{
			*account, lifecycle.value(u"store"_q).toString(), *generation, false};
		event.lifecycleMediaIdentity = lifecycle.value(u"media"_q).toString();
	}
	return Encode(event) == std::optional(bytes) ? std::optional(event) : std::nullopt;
}

std::optional<QByteArray> Read(
	const DesktopArchiveJournalAccess &access, const QString &name) {
	if (!access.current || !access.current()) {
		return {};
	}
	const auto bytes = DesktopArchiveRead(
		access.root + u"/outbox/"_q + name, kArchiveEnvelopeByteLimit + 4096);
	const auto opened
		= bytes ? OpenArchiveEnvelope({access.epoch.account, access.epoch.storeId,
										  ArchiveRecordKind::Event, name.toUtf8()},
					  access.security.keyId, *bytes, access.security.crypto)
				: ArchiveEnvelopeResult();
	return opened && access.current() ? std::optional(opened.bytes) : std::nullopt;
}

bool Write(const DesktopArchiveJournalAccess &access, const QString &name,
	const QByteArray &bytes) {
	if (!access.current || !access.current()) {
		return false;
	}
	const auto sealed = SealArchiveEnvelope({access.epoch.account, access.epoch.storeId,
												ArchiveRecordKind::Event, name.toUtf8()},
		access.security.keyId, bytes, access.security.crypto);
	return sealed && DesktopArchiveWrite(access.root + u"/outbox/"_q + name, sealed.bytes)
		   && access.current();
}

}

DesktopArchiveJournal::DesktopArchiveJournal(DesktopArchiveJournalAccess access)
	: _access(std::move(access)) {}

bool DesktopArchiveJournal::Open() {
	const auto lock = std::lock_guard(_mutex);
	if (!_access.current || !_access.current()) {
		_failure = u"outbox-stale"_q;
		return false;
	}
	const auto name = Token(_access, u"sequence"_q);
	const auto path = _access.root + u"/outbox/"_q + name;
	if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) {
		const auto bytes = Read(_access, name);
		const auto number = bytes ? Number(QString::fromLatin1(*bytes)) : std::nullopt;
		if (!number) {
			_failure = u"outbox-sequence"_q;
			return false;
		}
		_sequence = *number;
		if (_sequence < _access.minimumOrder) {
			_failure = u"outbox-order-regression"_q;
			return false;
		}
	} else {
		if (!QDir(_access.root + u"/outbox"_q).isEmpty()) {
			_failure = u"outbox-sequence-missing"_q;
			return false;
		}
		if (!Write(_access, name,
				QByteArray::number(qulonglong(_sequence = _access.minimumOrder)))) {
			_failure = u"outbox-sequence-create"_q;
			return false;
		}
	}
	_ready = true;
	return true;
}

bool DesktopArchiveJournal::Append(ArchiveEvent event) {
	const auto lock = std::lock_guard(_mutex);
	if (!_ready || !_access.current || !_access.current()
		|| event.key.account != _access.epoch.account || !IsArchiveToken(event.id)
		|| (event.lifecycleEpoch && *event.lifecycleEpoch != _access.epoch)) {
		_failure = u"outbox-not-ready"_q;
		return false;
	}
	const auto token = Token(_access, event.id);
	const auto path = _access.root + u"/outbox/"_q + token;
	if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) {
		const auto old = Read(_access, token);
		const auto pending = old ? Decode(*old) : std::nullopt;
		if (!pending || pending->id != event.id || pending->key != event.key
			|| pending->origin != event.origin || pending->content != event.content
			|| pending->generation != event.generation
			|| pending->targetVersion != event.targetVersion
			|| pending->targetRevision != event.targetRevision
			|| pending->expectedContentHash != event.expectedContentHash
			|| pending->lifecycleEpoch != event.lifecycleEpoch
			|| pending->lifecycleMediaIdentity != event.lifecycleMediaIdentity) {
			_failure = u"outbox-collision"_q;
			return false;
		}
		return true;
	}
	auto iterator = QDirIterator(_access.root + u"/outbox"_q,
		QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
	auto count = 0;
	while (iterator.hasNext()) {
		iterator.next();
		if (++count > 64) {
			_failure = u"outbox-full"_q;
			return false;
		}
	}
	if (_sequence == std::numeric_limits<uint64_t>::max()) {
		_failure = u"outbox-exhausted"_q;
		return false;
	}
	event.order = _sequence + 1;
	const auto bytes = Encode(event);
	if (!bytes) {
		_failure = u"outbox-version-too-large"_q;
		return false;
	}
	if (!Write(_access, Token(_access, u"sequence"_q),
			QByteArray::number(qulonglong(event.order)))) {
		_failure = u"outbox-sequence-write"_q;
		return false;
	}
	_sequence = event.order;
	if (!Write(_access, token, *bytes)) {
		_failure = u"outbox-append"_q;
		return false;
	}
	return true;
}

bool DesktopArchiveJournal::Drain(DesktopArchiveStore &store, int limit) {
	if (DrainPending(store, limit)) {
		return true;
	}
	auto reason = QString();
	const auto storageFailure = store.failure();
	{
		const auto lock = std::lock_guard(_mutex);
		if (_failure.isEmpty()) {
			_failure = storageFailure.isEmpty() ? u"outbox-replay-held"_q : storageFailure;
		}
		reason = _failure;
	}
	const auto observedAt = int64(QDateTime::currentSecsSinceEpoch());
	static_cast<void>(Attention(reason, observedAt));
	static_cast<void>(store.Gap(u"capture-replay-held"_q, reason, observedAt));
	return false;
}

bool DesktopArchiveJournal::DrainPending(DesktopArchiveStore &store, int limit) {
	if (limit < 1 || limit > 64 || store.epoch() != _access.epoch) {
		return false;
	}
	auto names = std::vector<QString>();
	auto sequence = uint64_t(0);
	{
		const auto lock = std::lock_guard(_mutex);
		if (!_ready || !_access.current || !_access.current()) {
			_failure = u"outbox-stale"_q;
			return false;
		}
		sequence = _sequence;
		auto iterator = QDirIterator(_access.root + u"/outbox"_q,
			QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
		while (iterator.hasNext()) {
			iterator.next();
			const auto name = iterator.fileName();
			if (name == Token(_access, u"sequence"_q)) {
				continue;
			}
			if (names.size() >= 64 || !IsArchiveHash(name)) {
				_failure = u"outbox-entry"_q;
				return false;
			}
			names.push_back(name);
		}
	}
	auto pending = std::vector<std::pair<QString, uint64_t>>();
	for (const auto &name : names) {
		const auto bytes = Read(_access, name);
		const auto event = bytes ? Decode(*bytes) : std::nullopt;
		if (!event || event->key.account != _access.epoch.account
			|| Token(_access, event->id) != name || event->order > sequence) {
			const auto lock = std::lock_guard(_mutex);
			_failure = u"outbox-evidence"_q;
			return false;
		}
		pending.push_back({name, event->order});
	}
	std::sort(pending.begin(), pending.end(),
		[](const auto &a, const auto &b) { return a.second < b.second; });
	for (auto i = size_t(0); i != pending.size() && i < size_t(limit); ++i) {
		const auto &name = pending[i].first;
		const auto bytes = Read(_access, name);
		const auto decoded = bytes ? Decode(*bytes) : std::nullopt;
		if (!decoded || decoded->order != pending[i].second
			|| (i && pending[i - 1].second == pending[i].second)) {
			return false;
		}
		auto event = *decoded;
		if (event.observation.context.value(u"desktopNonChannelDelete"_q).toBool()) {
			if (event.origin != ArchiveOrigin::AuthoritativeDelete
				|| event.key.chat != ArchiveChatKey{ArchiveChatKind::User, 1}) {
				return false;
			}
			const auto matches = store.NonChannelMessages(event.key.messageId);
			if (!matches) {
				return false;
			}
			if (matches->size() != 1) {
				if (!store.Gap(
						event.id, u"unresolved-delete"_q, event.observation.observedAt)) {
					return false;
				}
				const auto lock = std::lock_guard(_mutex);
				if (!_access.current()
					|| !DesktopArchiveRemoveFile(_access.root + u"/outbox/"_q + name)) {
					return false;
				}
				continue;
			}
			event.key = matches->front();
		}
		const auto slice = store.ReadSlice(event.key, event.id);
		if (!slice) {
			return false;
		}
		const auto message = FindArchiveMessage(slice->state, event.key);
		if (message && !message->versions.empty()
			&& event.observation.context.value(u"desktopSendMetadata"_q).toBool()) {
			const auto &head = message->versions.back();
			event.origin = ArchiveOrigin::SendMetadata;
			event.content.reset();
			event.missing = {};
			event.targetVersion = head.id;
			event.targetRevision = head.revision;
			event.expectedContentHash = ArchiveContentHash(head.content);
		}
		const auto preimageRetained
			= message && !message->versions.empty()
			  && event.origin == ArchiveOrigin::Baseline
			  && event.observation.context.value(u"desktopLivePreimage"_q).toBool();
		const auto committed
			= preimageRetained
			  || (message
				  && std::any_of(message->receipts.begin(), message->receipts.end(),
					  [&](const auto &receipt) { return receipt.id == event.id; }));
		if (!committed) {
			const auto captured = store.Capture(event);
			if (!captured.retained && captured.model != ArchivePlanStatus::Stale
				&& captured.model != ArchivePlanStatus::Purged
				&& captured.model != ArchivePlanStatus::NotFound) {
				return false;
			}
			if (!captured.retained
				&& !store.Gap(
					event.id, u"stale-capture"_q, event.observation.observedAt)) {
				return false;
			}
		}
		const auto lock = std::lock_guard(_mutex);
		if (!_access.current()
			|| !DesktopArchiveRemoveFile(_access.root + u"/outbox/"_q + name)) {
			_failure = u"outbox-confirmation"_q;
			return false;
		}
	}
	return true;
}

bool DesktopArchiveJournal::Attention(const QString &reason, int64 observedAt) {
	const auto lock = std::lock_guard(_mutex);
	return _ready && _access.current && _access.current()
		   && WriteDesktopArchiveAttention(
			   _access.root, _access.epoch, _access.security, reason, observedAt)
		   && _access.current();
}

QString DesktopArchiveJournal::failure() const {
	const auto lock = std::lock_guard(_mutex);
	return _failure;
}
const DesktopArchiveJournalAccess &DesktopArchiveJournal::access() const {
	return _access;
}

}

#include "purple/purple_sync_chat_carry_store.h"
#include "purple/purple_sync_chat_cache.h"
#include "purple/purple_sync_json.h"

#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <algorithm>
#include <limits>
#include <set>

namespace Purple {
namespace {

bool Hash(const QString &value) {
	if (value.size() != 64) { return false; }
	for (const auto ch : value) {
		if (!(ch >= u'0' && ch <= u'9') && !(ch >= u'a' && ch <= u'f')) { return false; }
	}
	return true;
}

QString StateHash(const std::optional<SyncLocalState> &state) {
	return state ? SyncCarryRecordHash(SerializeSyncLocalState(*state).canonical) : QString();
}

QString BindingHash(const QByteArray &value) {
	return SyncCarryRecordHash(value);
}

bool Same(const SyncPrivateSnapshot &a, const SyncPrivateSnapshot &b) {
	return a.status == SyncStoreStatus::Ready && b.status == SyncStoreStatus::Ready
		&& a.identity == b.identity && a.parentIdentity == b.parentIdentity && a.bytes == b.bytes;
}

bool CleanupShape(const QJsonValue &value, const SyncCarryJournal &j) {
	if (value.isUndefined()) { return true; }
	if (!value.isObject()) { return false; }
	const auto document=value.toObject();
	if (document.value(u"version"_q)!=QJsonValue(1) || !document.value(u"entries"_q).isObject()) { return false; }
	const auto entries=document.value(u"entries"_q).toObject(); if (entries.size()>1024 || !j.plan) { return false; }
	for (auto i=entries.begin();i!=entries.end();++i) {
		if (!IsSyncCarryHash(i.key()) || !i.value().isObject()) { return false; }
		const auto e=i.value().toObject(); const auto kind=e.value(u"kind"_q).toString();
		const auto id=e.value(u"id"_q); const auto date=DesktopSyncCarryTransportId(e.value(u"date"_q));
		const auto hash=e.value(u"hash"_q).toString(); const auto state=e.value(u"state"_q).toString();
		if (!id.isDouble() || id.toDouble()!=id.toInt() || id.toInt()<=0 || !date || *date>uint64_t(std::numeric_limits<int64_t>::max())
			|| (state!=u"issued"_q && state!=u"absent"_q)
			|| (e.contains(u"body_sha256"_q) && !IsSyncCarryHash(e.value(u"body_sha256"_q).toString()))
			|| (state==u"issued"_q && !e.contains(u"body_sha256"_q))) { return false; }
		if (kind==u"record"_q) {
			const auto found=std::find_if(j.plan->cleanupCandidates.begin(),j.plan->cleanupCandidates.end(),[&](const auto &v) {
				return v.reference.account==j.account && v.reference.chat==j.source && v.reference.messageId==id.toInt()
					&& v.reference.recordHash==hash && v.date==int64_t(*date);
			});
			if (found==j.plan->cleanupCandidates.end() || i.key()!=SyncCarryRecordHash((u"record:"_q+QString::number(id.toInt())+u":"_q+hash).toUtf8())) { return false; }
		} else if (kind==u"mark"_q) {
			if (!hash.isEmpty() || !j.removeOldMarkAuthorized || !j.oldMark || j.mode!=SyncCarryMode::Switch
				|| j.oldMark->markId!=id.toInt() || j.oldMark->markDate!=int64_t(*date)
				|| i.key()!=SyncCarryRecordHash((u"mark:"_q+QString::number(id.toInt())+u":"_q+QString::number(*date)).toUtf8())) { return false; }
		} else { return false; }
	}
	return true;
}

}

std::optional<uint64_t> DesktopSyncCarryTransportId(const QJsonValue &value) {
	if (!value.isString()) { return {}; }
	const auto text = value.toString();
	auto ok = false;
	const auto number = text.toULongLong(&ok);
	return ok && number && QString::number(number) == text ? std::optional<uint64_t>(number) : std::nullopt;
}

std::optional<QString> DesktopSyncCarryTransportHash(const QJsonObject &transport,
		const QJsonObject &envelope, const SyncCarryJournal &journal) {
	const auto source = PeerTokenText(journal.source, journal.account.userId);
	const auto target = PeerTokenText(journal.target, journal.account.userId);
	if (!source || !target) { return {}; }
	const auto scoped = QJsonObject{
		{u"domain"_q,u"purple-sync-carry-transport-v1"_q},
		{u"operation"_q,journal.operationId}, {u"mode"_q,int(journal.mode)},
		{u"source"_q,*source}, {u"target"_q,*target},
		{u"account"_q,envelope.value(u"account"_q)},
		{u"binding_sha256"_q,envelope.value(u"binding_sha256"_q)},
		{u"login_authority"_q,envelope.value(u"login_authority"_q)},
		{u"transport"_q,transport} };
	const auto encoded = CanonicalizeSyncJson(QJsonDocument(scoped).toJson(QJsonDocument::Compact));
	return encoded ? std::optional(SyncCarryRecordHash(encoded.canonical)) : std::nullopt;
}

std::optional<uint64_t> DesktopSyncCarryTransportGeneration(const QJsonObject &entry) {
	const auto value = entry.value(u"generation"_q);
	if (value.isUndefined()) { return uint64_t(0); }
	if (!value.isString()) { return {}; }
	const auto text = value.toString();
	auto ok = false;
	const auto generation = text.toULongLong(&ok);
	return ok && QString::number(generation) == text ? std::optional(generation) : std::nullopt;
}

bool DesktopSyncCarryTransportEntry(const QJsonObject &entry) {
	const auto kind = entry.value(u"kind"_q).toString();
	const auto state = entry.value(u"state"_q).toString();
	return DesktopSyncCarryTransportGeneration(entry).has_value()
		&& (kind == u"forward"_q || kind == u"own"_q || kind == u"mark"_q || kind == u"pending"_q)
		&& (state == u"prepared"_q || state == u"issued"_q || state == u"unknown"_q || state == u"verified"_q)
		&& IsSyncCarryHash(entry.value(u"hash"_q).toString())
		&& DesktopSyncCarryTransportId(entry.value(u"random_id"_q))
		&& ((kind == u"own"_q || kind == u"pending"_q) ? bool(DesktopSyncCarryTransportId(entry.value(u"file_id"_q))) : entry.value(u"file_id"_q).toString() == u"0"_q)
		&& entry.value(u"source_id"_q).isDouble()
		&& entry.value(u"source_id"_q).toDouble() >= 0
		&& entry.value(u"source_id"_q).toDouble() <= std::numeric_limits<int32_t>::max()
		&& entry.value(u"source_id"_q).toDouble() == entry.value(u"source_id"_q).toInt()
		&& (!entry.contains(u"ack_id"_q) || (entry.value(u"ack_id"_q).isDouble()
			&& entry.value(u"ack_id"_q).toDouble() > 0
			&& entry.value(u"ack_id"_q).toDouble() <= std::numeric_limits<int32_t>::max()
			&& entry.value(u"ack_id"_q).toDouble() == entry.value(u"ack_id"_q).toInt()))
		&& ((kind == u"forward"_q) == (entry.value(u"source_id"_q).toInt() > 0));
}

std::optional<bool> DesktopSyncCarryPendingIssued(
		const QJsonObject &envelope, const SyncCarryJournal &journal) {
	const auto transport = DesktopSyncCarryTransportOf(envelope, journal);
	if (!transport) { return {}; }
	const auto observed = transport->value(u"pending_intent_issued"_q);
	if (!observed.isUndefined() && !observed.isBool()) { return {}; }
	if (observed.toBool()) { return true; }
	const auto hash = envelope.value(u"opening_pending_sha256"_q).toString();
	for (const auto &value : transport->value(u"entries"_q).toObject()) {
		const auto entry = value.toObject();
		if (entry.value(u"kind"_q).toString() == u"pending"_q) {
			if (entry.value(u"hash"_q).toString() != hash) { return {}; }
			if (entry.value(u"state"_q).toString() != u"prepared"_q) { return true; }
		}
	}
	return false;
}

bool SyncChatCarryStore::RememberPendingIssued() {
	if (!Current() || !_journal || _journal->phase != SyncCarryPhase::ResolvePending || foreignLogin()) {
		return false;
	}
	const auto issued = DesktopSyncCarryPendingIssued(_envelope, *_journal);
	if (!issued) { return false; }
	if (*issued) { return true; }
	auto transport = DesktopSyncCarryTransportOf(_envelope, *_journal);
	transport->insert(u"pending_intent_issued"_q, true);
	const auto digest = DesktopSyncCarryTransportHash(*transport, _envelope, *_journal);
	if (!digest) { return false; }
	const auto before = _envelope;
	_envelope.insert(u"transport"_q, *transport);
	_envelope.insert(u"transport_sha256"_q, *digest);
	const auto written = Persist(SerializeSyncCarryJournal(*_journal));
	if (!written) { _envelope = before; }
	return bool(written);
}

std::optional<QJsonObject> DesktopSyncCarryTransportOf(
		const QJsonObject &envelope, const SyncCarryJournal &journal) {
	const auto value = envelope.value(u"transport"_q);
	if (value.isUndefined()) {
		return envelope.contains(u"transport_sha256"_q) ? std::nullopt
			: std::optional(QJsonObject{ {u"version"_q, 1}, {u"entries"_q, QJsonObject()} });
	}
	if (!value.isObject()) { return {}; }
	const auto result = value.toObject();
	if (!result.value(u"version"_q).isDouble() || result.value(u"version"_q).toDouble() != 1
		|| !result.value(u"entries"_q).isObject() || result.value(u"entries"_q).toObject().size() > 1024) { return {}; }
	if (!CleanupShape(result.value(u"cleanup"_q),journal)) { return {}; }
	if (result.contains(u"cleanup_settings_authorized"_q) && (!result.value(u"cleanup_settings_authorized"_q).isBool()
		|| (result.value(u"cleanup_settings_authorized"_q).toBool() && journal.mode!=SyncCarryMode::Switch))) { return {}; }
	const auto digest = DesktopSyncCarryTransportHash(result,envelope,journal);
	if (!digest || envelope.value(u"transport_sha256"_q).toString() != *digest) { return {}; }
	const auto entries = result.value(u"entries"_q).toObject();
	auto randoms = std::set<uint64_t>();
	for (auto i = entries.begin(); i != entries.end(); ++i) {
		if (!IsSyncCarryHash(i.key()) || !i.value().isObject() || !DesktopSyncCarryTransportEntry(i.value().toObject())) { return {}; }
		const auto entry = i.value().toObject();
		const auto key = SyncCarryRecordHash((entry.value(u"kind"_q).toString() + u":"_q
			+ QString::number(entry.value(u"source_id"_q).toInt()) + u":"_q + entry.value(u"hash"_q).toString()).toUtf8());
		if (i.key() != key || !randoms.emplace(*DesktopSyncCarryTransportId(entry.value(u"random_id"_q))).second) { return {}; }
		const auto random = DesktopSyncCarryTransportId(entry.value(u"random_id"_q));
		const auto own = DesktopSyncCarryTransportId(journal.ownRequestId);
		const auto mark = DesktopSyncCarryTransportId(journal.markRequestId);
		const auto kind = entry.value(u"kind"_q).toString();
		if ((kind == u"own"_q && random != own) || (kind == u"mark"_q && random != mark)
			|| (kind == u"forward"_q && (random == own || (mark && random == mark)))) { return {}; }
	}
	if (!entries.isEmpty() && (!result.value(u"readers"_q).isObject() || !result.value(u"reader_warning"_q).isBool())) { return {}; }
	return result;
}

bool DesktopSyncCarryReaderAuthority(const QJsonObject &snapshot, const DesktopSyncChatPeer &peer) {
	const auto known = snapshot.value(u"members_known"_q);
	return peer.facts && snapshot.value(u"members"_q).isArray()
		&& snapshot.value(u"other_members"_q).isString() && snapshot.value(u"other_admins"_q).isString()
		&& snapshot.value(u"warning"_q).isBool()
		&& (known.isUndefined() || known.isBool()) && (known != QJsonValue(true) || peer.readerMembers)
		&& (snapshot.value(u"creator"_q) != QJsonValue(true) || peer.creator == true)
		&& (snapshot.value(u"admin"_q) != QJsonValue(true) || peer.facts->admin)
		&& snapshot.value(u"kind"_q).toInt(-1) == int(peer.kind)
		&& snapshot.value(u"settings_disclosed"_q).toBool()
		&& (snapshot.value(u"stream"_q) == QJsonValue(u"config"_q) || snapshot.value(u"stream"_q) == QJsonValue(u"library"_q));
}

QString DesktopSyncCarryLogin(const QString &loginBinding) {
	return Hash(loginBinding) ? SyncCarryRecordHash("purple-sync-carry-login-v2:" + loginBinding.toLatin1()).left(32) : QString();
}

SyncChatCarryStore::SyncChatCarryStore(QString directory, SyncChatCarryStoreScope scope,
		SyncDurableFault fault)
: _directory(std::move(directory))
, _scope(std::move(scope))
, _fault(std::move(fault)) {
}

SyncChatCarryStore::~SyncChatCarryStore() { Close(); }

void SyncChatCarryStore::Close() {
	_ioEpoch->fetch_add(1);
	_ioActive.clear();
	_ioReadFences.clear();
	_ioReadOpened = {};
	_local.reset();
	_thread = nullptr;
}

bool SyncChatCarryStore::Current() const {
	return !_failed && _local && _thread == QThread::currentThread() && SyncStoreWorkerThread()
		&& SyncPathIdentity(_directory) == _directoryIdentity;
}

bool SyncChatCarryStore::StateMatches() const {
	return Current() && Same(_stateFile,
		CaptureSyncPrivateFile(_directory + u"/state.json"_q, kSyncCarryJournalStorageLimit));
}

SyncChatCarryStorageResult SyncChatCarryStore::Fail(SyncStoreStatus status) {
	_lastFailure = status;
	_failed = true;
	Close();
	return { status };
}

SyncChatCarryStorageResult SyncChatCarryStore::Open() {
	if (!SyncStoreWorkerThread()) { return { SyncStoreStatus::WrongThread }; }
	if (_local || !AccountKeyText(_scope.account.environment, _scope.account.userId)
		|| !Hash(_scope.loginAuthority) || !QDir::isAbsolutePath(_directory)) {
		return { SyncStoreStatus::InvalidTransition };
	}
	_ioReadOpened = DesktopSyncChatReadStamp::Begin();
	if (!_ioReadOpened.valid()) { return {SyncStoreStatus::InvalidTransition}; }
	_failed = false; _lastFailure = SyncStoreStatus::Ready;
	_objects = {}; _envelope = {}; _journal.reset(); _objectBytes = 0; _foreignLogin = false;
	_thread = QThread::currentThread();
	_local = std::make_unique<SyncLocalStore>(_directory);
	const auto opened = _local->OpenForCarry(_scope.binding);
	if (opened.status != SyncStoreStatus::Ready && opened.status != SyncStoreStatus::Uninitialized) {
		return Fail(opened.status);
	}
	_directoryIdentity = SyncPathIdentity(_directory);
	_stateFile = CaptureSyncPrivateFile(_directory + u"/state.json"_q, kSyncCarryJournalStorageLimit);
	if (_stateFile.status != SyncStoreStatus::Ready || _directoryIdentity.isEmpty()
		|| _directoryIdentity == "missing") { return Fail(SyncStoreStatus::IoError); }
	_cacheFile = CaptureSyncPrivateFile(_scope.cacheFile, 64 * 1024);
	if (_cacheFile.status != SyncStoreStatus::Ready) { return Fail(_cacheFile.status); }
	if (!_cacheFile.absent()) {
		const auto cache = ParseDesktopSyncChatCache(_cacheFile.bytes, _scope.account);
		if (cache.status == DesktopSyncChatCacheStatus::Valid && cache.paused) { return Fail(SyncStoreStatus::InvalidTransition); }
	}
	const auto path = _directory + u"/carry.json"_q;
	_journalFile = CaptureSyncPrivateFile(path, kSyncCarryJournalStorageLimit);
	if (_journalFile.status != SyncStoreStatus::Ready) { return Fail(_journalFile.status); }
	if (_journalFile.absent()) {
		const auto objects = LoadObjects(); if (objects != SyncStoreStatus::Ready) { return Fail(objects); }
		if (!RetainedEffects()) { return Fail(SyncStoreStatus::InvalidState); }
		return _local->_state && CheckSyncAccountBinding(*_local->_state, _scope.binding) != SyncAccountBindingVerdict::Bound
			? Fail(SyncStoreStatus::AccountUnbound) : SyncChatCarryStorageResult{SyncStoreStatus::Ready};
	}
	const auto canonical = CanonicalizeSyncJson(_journalFile.bytes);
	if (!canonical) { return Fail(SyncStoreStatus::InvalidState); }
	_envelope = QJsonDocument::fromJson(canonical.canonical).object();
	const auto version = _envelope.value(u"version"_q);
	if (version.isDouble() && version.toDouble() > 1) { return Fail(SyncStoreStatus::NewerState); }
	if (!version.isDouble() || version.toDouble() != 1) { return Fail(SyncStoreStatus::InvalidState); }
	const auto account = AccountKeyText(_scope.account.environment, _scope.account.userId);
	if (_envelope.value(u"account"_q).toString() != *account
		|| !Hash(_envelope.value(u"login_authority"_q).toString())) {
		return Fail(SyncStoreStatus::AccountUnbound);
	}
	if (!_envelope.value(u"journal"_q).isObject() || !_envelope.value(u"objects"_q).isObject()) {
		return Fail(SyncStoreStatus::InvalidState);
	}
	const auto journal = ParseSyncCarryJournal(QJsonDocument(_envelope.value(u"journal"_q).toObject()).toJson(QJsonDocument::Compact));
	if (!journal || journal.journal.account != _scope.account
		|| (_envelope.value(u"binding_sha256"_q).toString() != BindingHash(_scope.binding)
			&& !(_scope.binding.isEmpty() && _local->_state
				&& _envelope.value(u"login_authority"_q).toString() != _scope.loginAuthority
				&& _envelope.value(u"binding_sha256"_q).toString() == BindingHash(_local->_state->bindingToken.toLatin1()))
			&& !(_envelope.value(u"binding_sha256"_q).toString() == BindingHash({})
				&& journal.journal.mode == SyncCarryMode::Fill && journal.journal.stateHash.isEmpty()
				&& !SyncCarryJournalHoldsWrites(journal.journal)))
		|| _envelope.value(u"operation"_q).toString() != journal.journal.operationId
		|| _envelope.value(u"journal_sha256"_q).toString() != SyncCarryRecordHash(journal.canonical)) {
		return Fail(journal.status == SyncCarryJournalStatus::NewerVersion
			? SyncStoreStatus::NewerState : SyncStoreStatus::InvalidState);
	}
	_objects = _envelope.value(u"objects"_q).toObject();
	for (auto i = _objects.begin(); i != _objects.end(); ++i) { if (!ReadObject(i.value().toString(), i.key())) { return Fail(SyncStoreStatus::InvalidState); } }
	const auto objects = LoadObjects(); if (objects != SyncStoreStatus::Ready) { return Fail(objects); }
	if (_local->_state && !_scope.binding.isEmpty()
		&& CheckSyncAccountBinding(*_local->_state, _scope.binding) != SyncAccountBindingVerdict::Bound) { return Fail(SyncStoreStatus::AccountUnbound); }
	_journal = journal.journal;
	_foreignLogin = _envelope.value(u"login_authority"_q).toString() != _scope.loginAuthority;
	if (_journal->version != 2 || _journal->loginAuthority != DesktopSyncCarryLogin(_envelope.value(u"login_authority"_q).toString())) {
		return Fail(SyncStoreStatus::AccountUnbound);
	}
	if (!RetainedEffects() || !CleanupLedger()) { return Fail(SyncStoreStatus::InvalidState); }
	return { SyncStoreStatus::Ready, journal.status };
}

SyncStoreStatus SyncChatCarryStore::LoadObjects() {
	_objectBytes = 0;
	auto objects = QJsonObject();
	const auto directory = _directory + u"/carry-objects"_q;
	if (SyncPathIdentity(directory) != "missing" && (QFileInfo(directory).isSymLink() || !QFileInfo(directory).isDir())) { return SyncStoreStatus::InsecurePermissions; }
	auto entries = QDirIterator(directory, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
	while (entries.hasNext()) {
		const auto name = QFileInfo(entries.next()).fileName();
		const auto separator = name.indexOf(u'-');
		if (separator <= 0 || !name.endsWith(u".json"_q)) { return SyncStoreStatus::InvalidState; }
		const auto kind = name.left(separator);
		const auto hash = name.mid(separator + 1, name.size() - separator - 6);
		const auto bytes = ReadObject(kind, hash);
		if (!bytes || (objects.contains(hash) && objects.value(hash).toString() != kind)) { return SyncStoreStatus::InvalidState; }
		objects.insert(hash, kind); _objectBytes += bytes->size();
		if (objects.size() > 1024 || _objectBytes > kSyncCarryObjectStorageLimit) { return SyncStoreStatus::RetentionFull; }
	}
	_objects = std::move(objects);
	return SyncStoreStatus::Ready;
}

QString SyncChatCarryStore::ObjectPath(const QString &kind, const QString &hash) const {
	return _directory + u"/carry-objects/"_q + kind + u'-' + hash + u".json"_q;
}

std::optional<QByteArray> SyncChatCarryStore::ReadObject(const QString &kind, const QString &hash) const {
	if (!Hash(hash) || (kind != u"state"_q && kind != u"record"_q && kind != u"text"_q)) { return {}; }
	auto bytes = QByteArray();
	const auto limit = kind == u"state"_q ? kSyncCarryJournalStorageLimit : kSyncSettingsRecordStorageLimit;
	if (ReadSyncPrivateFile(ObjectPath(kind, hash), limit, bytes) != SyncStoreStatus::Ready
		|| SyncCarryRecordHash(bytes) != hash) { return {}; }
	return bytes;
}

SyncChatCarryStorageResult SyncChatCarryStore::StoreObject(const QString &kind, const QByteArray &bytes) {
	if (!StateMatches()) { return { SyncStoreStatus::Changed }; }
	const auto limit = kind == u"state"_q ? kSyncCarryJournalStorageLimit : kSyncSettingsRecordStorageLimit;
	if (bytes.size() > limit) { return { SyncStoreStatus::BaseRecordTooLarge }; }
	const auto hash = SyncCarryRecordHash(bytes);
	if (_objects.contains(hash) && _objects.value(hash).toString() != kind) { return { SyncStoreStatus::InvalidState }; }
	if (!_objects.contains(hash) && (_objectBytes + bytes.size() > kSyncCarryObjectStorageLimit || _objects.size() == 1024)) { return { SyncStoreStatus::RetentionFull }; }
	const auto directory = _directory + u"/carry-objects"_q;
	const auto created = EnsureSyncPrivateDirectory(directory);
	if (created != SyncStoreStatus::Ready) { return { created }; }
	if (SyncPrivateDirectoryBarrier(_directory) != SyncStoreStatus::Ready) { return Fail(SyncStoreStatus::DurabilityFailed); }
	const auto path = ObjectPath(kind, hash);
	const auto expected = CaptureSyncPrivateFile(path, limit);
	if (expected.status != SyncStoreStatus::Ready || (!expected.absent() && expected.bytes != bytes)) {
		return { SyncStoreStatus::Changed };
	}
	const auto status = expected.absent() ? WriteSyncDurableFile(path, bytes, expected, _fault)
		: ConfirmSyncDurableFile(path, expected, _fault);
	if (status != SyncStoreStatus::Ready) { return Fail(status); }
	if (!_objects.contains(hash)) { _objectBytes += bytes.size(); }
	_objects.insert(hash, kind);
	return { SyncStoreStatus::Ready, SyncCarryJournalStatus::Valid, hash };
}

SyncChatCarryStorageResult SyncChatCarryStore::PrepareState(const SyncLocalState &state) {
	if (CheckSyncAccountBinding(state, _scope.binding) != SyncAccountBindingVerdict::Bound) {
		return { SyncStoreStatus::AccountUnbound };
	}
	const auto retention = ValidateSyncRetainedConfig(state);
	if (retention != SyncStoreStatus::Ready) { return { retention }; }
	if (!state.configData.base.isEmpty() && !FindBase(state)) { return { SyncStoreStatus::MissingLocalVersion }; }
	const auto encoded = SerializeSyncLocalState(state);
	return encoded ? StoreObject(u"state"_q, encoded.canonical)
		: SyncChatCarryStorageResult{ SyncStoreStatus::InvalidState };
}

SyncChatCarryStorageResult SyncChatCarryStore::Persist(const SyncCarryJournalResult &next) {
	if (!StateMatches() || next.canonical.isEmpty()
		|| !SerializeSyncCarryJournal(next.journal)
		|| SerializeSyncCarryJournal(next.journal).canonical != next.canonical
		|| next.journal.account != _scope.account) { return { SyncStoreStatus::InvalidTransition }; }
	auto document = _envelope;
	document.insert(u"version"_q, 1);
	document.insert(u"account"_q, *AccountKeyText(_scope.account.environment, _scope.account.userId));
	if (!document.contains(u"binding_sha256"_q)) { document.insert(u"binding_sha256"_q, BindingHash(_scope.binding)); }
	if (!document.contains(u"login_authority"_q)) { document.insert(u"login_authority"_q, _scope.loginAuthority); }
	document.insert(u"operation"_q, next.journal.operationId);
	document.insert(u"journal_sha256"_q, SyncCarryRecordHash(next.canonical));
	document.insert(u"journal"_q, QJsonDocument::fromJson(next.canonical).object());
	document.insert(u"objects"_q, _objects);
	const auto encoded = CanonicalizeSyncJson(QJsonDocument(document).toJson(QJsonDocument::Compact));
	if (!encoded || encoded.canonical.size() > kSyncCarryJournalStorageLimit) { return { SyncStoreStatus::RetentionFull }; }
	const auto path = _directory + u"/carry.json"_q;
	const auto status = WriteSyncDurableFile(path, encoded.canonical, _journalFile, _fault);
	if (status != SyncStoreStatus::Ready) { return Fail(status); }
	_journalFile = CaptureSyncPrivateFile(path, kSyncCarryJournalStorageLimit);
	if (_journalFile.status != SyncStoreStatus::Ready || _journalFile.bytes != encoded.canonical
		|| _journalFile.parentIdentity != _directoryIdentity) { return Fail(SyncStoreStatus::Changed); }
	if (!_journal || SerializeSyncCarryJournal(*_journal).canonical != next.canonical) { _ioEpoch->fetch_add(1); }
	_envelope = document; _journal = next.journal;
	if (!SyncCarryJournalHoldsWrites(*_journal)) {
		const auto archived = ArchiveTerminal(false); if (!archived) { return archived; }
	}
	_lastFailure = SyncStoreStatus::Ready;
	return { SyncStoreStatus::Ready, next.status };
}

bool SyncChatCarryStore::WitnessCleanup() {
	if (!Current() || !_journal || !_journal->plan || foreignLogin() || _journal->phase!=SyncCarryPhase::Cleanup
		|| !_journal->stateCommitted || !_journal->cacheCompleted || !_journal->targetMark
		|| StateHash(_local->_state)!=_journal->nextStateHash) { return false; }
	const auto state=WitnessState(_journal->nextStateHash);
	if (state!=SyncStoreStatus::Ready) { static_cast<void>(Fail(state)); return false; }
	if (!Same(_cacheFile,CaptureSyncPrivateFile(_scope.cacheFile,64*1024))) { static_cast<void>(Fail(SyncStoreStatus::Changed)); return false; }
	const auto cache=WitnessCache({_journal->operationId,_scope.account,*_journal->targetMark,true});
	if (cache!=SyncStoreStatus::Ready) { static_cast<void>(Fail(cache)); return false; }
	return bool(CleanupLedger());
}

std::optional<QJsonObject> SyncChatCarryStore::CleanupLedger() const {
	if (!Current() || !_journal) { return {}; }
	const auto transport=DesktopSyncCarryTransportOf(_envelope,*_journal); if (!transport) { return {}; }
	const auto entries=transport->value(u"cleanup"_q).toObject().value(u"entries"_q).toObject();
	for (const auto &v:entries) {
		const auto e=v.toObject(); const auto hash=e.value(u"body_sha256"_q).toString();
		if (!hash.isEmpty() && !ReadObject(e.value(u"kind"_q)==QJsonValue(u"mark"_q)?u"text"_q:u"record"_q,hash)) { return {}; }
	}
	return entries;
}

SyncChatCarryStorageResult SyncChatCarryStore::SaveCleanupLedger(const QJsonObject &entries) {
	if (!WitnessCleanup()) { return {Current()?SyncStoreStatus::InvalidTransition:_lastFailure}; }
	auto transport=DesktopSyncCarryTransportOf(_envelope,*_journal); if (!transport) { return {SyncStoreStatus::InvalidState}; }
	auto value=transport->value(u"cleanup"_q).toObject();
	value.insert(u"version"_q,1); value.insert(u"entries"_q,entries);
	if (!CleanupShape(value,*_journal)) { return {SyncStoreStatus::InvalidState}; }
	transport->insert(u"cleanup"_q,value);
	const auto hash=DesktopSyncCarryTransportHash(*transport,_envelope,*_journal); if (!hash) { return {SyncStoreStatus::InvalidState}; }
	const auto before=_envelope;
	_envelope.insert(u"transport"_q,*transport); _envelope.insert(u"transport_sha256"_q,*hash);
	const auto written=Persist(SerializeSyncCarryJournal(*_journal));
	if (!written) { _envelope=before; _lastFailure=written.status; }
	return written;
}

SyncChatCarryStorageResult SyncChatCarryStore::Begin(const SyncCarryJournalStart &input, bool consentRequired, bool cleanupSettings) {
	const auto retained = RetainedEffects();
	if (!retained) { return { SyncStoreStatus::InvalidState }; }
	for (const auto &old : *retained) {
		if (old.operationId == input.operationId) { return { SyncStoreStatus::InvalidTransition }; }
		for (const auto &request : {old.markRequestId, old.ownRequestId}) {
			if (!request.isEmpty() && (request == input.markRequestId || request == input.ownRequestId)) { return { SyncStoreStatus::InvalidTransition }; }
		}
	}
	const auto images = RetainedImages();
	if (!images) { return { SyncStoreStatus::InvalidState }; }
	for (const auto &bytes : *images) {
		const auto envelope = QJsonDocument::fromJson(bytes).object();
		for (const auto &value : envelope.value(u"transport"_q).toObject().value(u"entries"_q).toObject()) {
			const auto request = value.toObject().value(u"random_id"_q).toString();
			if (!request.isEmpty() && (request == input.markRequestId || request == input.ownRequestId)) { return { SyncStoreStatus::InvalidTransition }; }
		}
	}
	if (_journal && !SyncCarryJournalHoldsWrites(*_journal)) {
		const auto archived = ArchiveTerminal(); if (!archived) { return archived; }
	}
	if (!StateMatches() || _journal || input.account != _scope.account
		|| input.preferenceToken != _scope.binding || StateHash(input.state) != StateHash(_local->_state)
		|| (!input.state && (!input.stateAbsentConfirmed || !_stateFile.absent()))) {
		return { SyncStoreStatus::InvalidTransition };
	}
	if (!_cacheFile.absent()) {
		const auto cache = ParseDesktopSyncChatCache(_cacheFile.bytes, _scope.account);
		if (cache.status != DesktopSyncChatCacheStatus::Valid || cache.paused) { return { SyncStoreStatus::InvalidState }; }
	}
	auto start = input;
	start.loginAuthority = DesktopSyncCarryLogin(_scope.loginAuthority);
	const auto directory = _directory + u"/pending"_q;
	auto entries = QDirIterator(directory, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
	while (entries.hasNext()) {
		const auto path = entries.next();
		const auto expected = _local->_state && _local->_state->config.pendingSeq
			? directory + u"/config-"_q + QString::number(_local->_state->config.pendingSeq) + u".json"_q : QString();
		if (path != expected) { start.otherStagesPresent = true; }
	}
	if (_local->_state && _local->_state->config.pendingSeq) {
		const auto pending = _local->ReadPendingConfigLocked();
		if (!pending) { return { pending.status }; }
		const auto retained = StoreObject(u"record"_q, pending.staged);
		if (!retained) { return retained; }
		_envelope.insert(u"opening_pending_sha256"_q, SyncCarryRecordHash(pending.staged));
	}
	if (consentRequired) { _envelope.insert(u"consent_required"_q, true); }
	const auto next = BeginSyncCarryJournal(start);
	if (!next) { return { SyncStoreStatus::InvalidTransition, next.status }; }
	if (_local->_state) {
		if (!_local->_state->configData.base.isEmpty() && !OwnBase()) { return { SyncStoreStatus::MissingLocalVersion }; }
		if (!_local->_state->configData.base.isEmpty()) {
			const auto record = StoreObject(u"record"_q, OwnBase().staged); if (!record) { return record; }
		}
		const auto prepared = PrepareState(*_local->_state); if (!prepared) { return prepared; }
	}
	if (cleanupSettings) {
		if (input.mode!=SyncCarryMode::Switch || !input.operationConfirmed) { return {SyncStoreStatus::InvalidTransition}; }
		const auto transport=QJsonObject{{u"version"_q,1},{u"entries"_q,QJsonObject()},{u"cleanup_settings_authorized"_q,true}};
		const auto hash=DesktopSyncCarryTransportHash(transport,QJsonObject{{u"account"_q,*AccountKeyText(_scope.account.environment,_scope.account.userId)},
			{u"binding_sha256"_q,BindingHash(_scope.binding)},{u"login_authority"_q,_scope.loginAuthority}},next.journal);
		if (!hash) { return {SyncStoreStatus::InvalidState}; }
		_envelope.insert(u"transport"_q,transport); _envelope.insert(u"transport_sha256"_q,*hash);
	}
	return Persist(next);
}

SyncStoreStatus SyncChatCarryStore::WitnessState(const QString &hash) {
	if (!StateMatches()) { return SyncStoreStatus::Changed; }
	if (hash.isEmpty()) {
		if (!_stateFile.absent()) { return SyncStoreStatus::Changed; }
		if (_fault && !_fault(SyncDurablePoint::DirectoryBarrier)) { return SyncStoreStatus::DurabilityFailed; }
		return SyncPrivateDirectoryBarrier(_directory);
	}
	const auto image = ReadObject(u"state"_q, hash);
	if (!image || _stateFile.absent() || _stateFile.bytes != *image) { return SyncStoreStatus::Changed; }
	return ConfirmSyncDurableFile(_directory + u"/state.json"_q, _stateFile, _fault);
}

SyncStoreStatus SyncChatCarryStore::WitnessCache(const SyncCarryCacheReceipt &receipt) {
	if (!_journal || !_journal->targetMark || receipt.operationId != _journal->operationId
		|| receipt.account != _scope.account || receipt.mark != *_journal->targetMark) { return SyncStoreStatus::InvalidTransition; }
	const auto path = receipt.complete ? _scope.cacheFile : _directory + u"/carry-cache.json"_q;
	if (path.isEmpty() || (receipt.complete && QFileInfo(path).fileName() != DesktopSyncChatCacheFileName(_scope.account))) {
		return SyncStoreStatus::InvalidTransition;
	}
	const auto snapshot = CaptureSyncPrivateFile(path, 64 * 1024);
	if (snapshot.status != SyncStoreStatus::Ready || snapshot.absent()) { return SyncStoreStatus::IoError; }
	const auto parsed = ParseDesktopSyncChatCache(snapshot.bytes, _scope.account);
	if (!parsed.record || parsed.record->loginBinding != _scope.loginAuthority
		|| parsed.record->mark != receipt.mark
		|| (receipt.complete ? parsed.record->carry.has_value()
			: (!parsed.record->carry || parsed.record->carry->value(u"operation"_q).toString() != receipt.operationId))) {
		return SyncStoreStatus::InvalidState;
	}
	return ConfirmSyncDurableFile(path, snapshot, _fault);
}

SyncChatCarryStorageResult SyncChatCarryStore::Apply(const SyncCarryJournalEvent &event) {
	if (!StateMatches() || !_journal) { return { SyncStoreStatus::Changed }; }
	const auto next = AdvanceSyncCarryJournal(*_journal, event, DesktopSyncCarryLogin(_scope.loginAuthority));
	return !next.canonical.isEmpty() ? Persist(next) : SyncChatCarryStorageResult{ SyncStoreStatus::InvalidTransition, next.status };
}

SyncChatCarryStorageResult SyncChatCarryStore::Advance(SyncCarryJournalEvent event) {
	if (!StateMatches() || !_journal || _foreignLogin || event.kind == SyncCarryEventKind::StateObserved
		|| event.kind == SyncCarryEventKind::StageRetired || event.kind == SyncCarryEventKind::OwnStagePersisted) {
		return { SyncStoreStatus::InvalidTransition };
	}
	if (event.operationId != _journal->operationId || event.revision != _journal->revision
		|| event.account != _scope.account || event.source != _journal->source || event.target != _journal->target) {
		return { SyncStoreStatus::InvalidTransition };
	}
	event.objectsDurable = false;
	event.state = _local->_state;
	event.stateAbsentConfirmed = _stateFile.absent();
	if (event.outcome != SyncCarryOutcome::Success) { return Apply(event); }
	if (event.kind == SyncCarryEventKind::PlanPrepared && event.plan) {
		if (_journal->phase != SyncCarryPhase::Plan) { return { SyncStoreStatus::InvalidTransition }; }
		if (!event.plan->ownRecord.isEmpty()) {
			if (!_local->_state) { return { SyncStoreStatus::InvalidState }; }
			const auto reserved = PrepareSyncCarryStage(*event.plan, *_local->_state);
			if (!reserved) { return { SyncStoreStatus::InvalidTransition }; }
			const auto record = StoreObject(u"record"_q, event.plan->ownRecord); if (!record) { return record; }
			const auto image = PrepareState(*reserved); if (!image) { return image; }
		}
		event.objectsDurable = true;
	} else if (event.kind == SyncCarryEventKind::PendingPrepared && _local->_state) {
		if (event.pendingSendIntent == SyncCarrySendIntent::Issued && !RememberPendingIssued()) {
			return { SyncStoreStatus::InvalidState };
		}
		const auto issued = DesktopSyncCarryPendingIssued(_envelope, *_journal);
		if (!issued) { return { SyncStoreStatus::InvalidState }; }
		if (*issued) { event.pendingSendIntent = SyncCarrySendIntent::Issued; }
		const auto pending = event.pendingAccess == SyncCarrySourceAccess::Protected
			? ReconcileProtectedStageForCarry(_scope.account, *_local->_state, _journal->source,
				event.pendingAccess, event.pendingSendIntent, event.stagedRecord, event.retainedLocalText, event.pendingInventory)
			: event.pendingReadback
			? FinishPendingStageForCarry(_scope.account, *_local->_state, event.stagedRecord, *event.pendingReadback)
			: AbandonStageForLostChat(_scope.account, *_local->_state, _journal->source, event.oldRead, event.stagedRecord, event.retainedLocalText);
		if (!pending) { return { SyncStoreStatus::InvalidTransition }; }
		const auto path = _directory + u"/pending/config-"_q + QString::number(_journal->pendingSeq) + u".json"_q;
		const auto staged = CaptureSyncPrivateFile(path, kSyncSettingsRecordStorageLimit);
		if (staged.status != SyncStoreStatus::Ready || staged.absent() || staged.bytes != event.stagedRecord) { return { SyncStoreStatus::PendingMismatch }; }
		const auto record = StoreObject(u"record"_q, event.stagedRecord); if (!record) { return record; }
		if (!event.retainedLocalText.isEmpty()) { const auto text = StoreObject(u"text"_q, event.retainedLocalText); if (!text) { return text; } }
		const auto prepared = PrepareState(pending.state); if (!prepared) { return prepared; }
		event.preparedImageHash = prepared.objectHash; event.objectsDurable = true;
	} else if (event.kind == SyncCarryEventKind::DestinationRead && event.destination && _journal->plan) {
		const auto prepared = PrepareSyncCarryJournalCommit(*_journal, _local->_state, *event.destination, event.playlist);
		const auto &commit = prepared.commit;
		if (!commit) { return Apply(event); }
		if (commit.state) { const auto image = PrepareState(*commit.state); if (!image) { return image; } }
		if (!prepared.targetPendingRecord.isEmpty()) {
			const auto record = StoreObject(u"record"_q, prepared.targetPendingRecord); if (!record) { return record; }
		}
		event.targetPendingRecord = prepared.targetPendingRecord;
		event.preparedImageHash = commit.stateHash; event.objectsDurable = true;
	} else if (event.kind == SyncCarryEventKind::CachePrepared || event.kind == SyncCarryEventKind::CacheCompleted) {
		if (event.kind == SyncCarryEventKind::CacheCompleted) {
			const auto pending = InstallTargetPending(); if (pending != SyncStoreStatus::Ready) { return Fail(pending); }
			event.targetPendingStageDurable = !_journal->targetPendingRecord.isEmpty();
			event.stagedRecord = _journal->targetPendingRecord;
		}
		const auto status = WitnessCache(event.cache);
		if (status != SyncStoreStatus::Ready) { return Fail(status); }
		event.objectsDurable = true;
	}
	return Apply(event);
}

SyncChatCarryStorageResult SyncChatCarryStore::StageOwn() {
	if (!StateMatches() || _foreignLogin || !_journal || !_journal->plan || !_local->_state
		|| NextSyncCarryJournalAction(*_journal) != SyncCarryJournalAction::StageOwn) { return { SyncStoreStatus::InvalidTransition }; }
	const auto state = PrepareSyncCarryStage(*_journal->plan, *_local->_state);
	if (!state) { return { SyncStoreStatus::InvalidTransition }; }
	const auto record = StoreObject(u"record"_q, _journal->plan->ownRecord); if (!record) { return record; }
	const auto image = PrepareState(*state); if (!image) { return image; }
	const auto directory = _directory + u"/pending"_q;
	const auto created = EnsureSyncPrivateDirectory(directory); if (created != SyncStoreStatus::Ready) { return { created }; }
	if (SyncPrivateDirectoryBarrier(_directory) != SyncStoreStatus::Ready) { return Fail(SyncStoreStatus::DurabilityFailed); }
	const auto path = directory + u"/config-"_q + QString::number(state->config.pendingSeq) + u".json"_q;
	const auto expected = CaptureSyncPrivateFile(path, kSyncSettingsRecordStorageLimit);
	if (expected.status != SyncStoreStatus::Ready || (!expected.absent() && expected.bytes != _journal->plan->ownRecord)) { return { SyncStoreStatus::OrphanStage }; }
	auto status = expected.absent() ? WriteSyncDurableFile(path, _journal->plan->ownRecord, expected, _fault)
		: ConfirmSyncDurableFile(path, expected, _fault);
	if (status != SyncStoreStatus::Ready) { return Fail(status); }
	const auto bytes = SerializeSyncLocalState(*state).canonical;
	status = WriteSyncDurableFile(_directory + u"/state.json"_q, bytes, _stateFile, _fault);
	if (status != SyncStoreStatus::Ready) { return Fail(status); }
	_stateFile = CaptureSyncPrivateFile(_directory + u"/state.json"_q, kSyncCarryJournalStorageLimit);
	if (_stateFile.status != SyncStoreStatus::Ready || _stateFile.bytes != bytes) { return Fail(SyncStoreStatus::Changed); }
	_local->_state = *state;
	auto event = SyncCarryEvent(*_journal, SyncCarryEventKind::OwnStagePersisted);
	event.outcome = SyncCarryOutcome::Success;
	event.state = *state; event.stagedRecord = _journal->plan->ownRecord; event.objectsDurable = true;
	return Apply(event);
}

SyncChatCarryStorageResult SyncChatCarryStore::CommitPreparedState() {
	if (!StateMatches() || _foreignLogin || !_journal) { return { SyncStoreStatus::InvalidTransition }; }
	const auto action = NextSyncCarryJournalAction(*_journal);
	if (action != SyncCarryJournalAction::WritePendingState && action != SyncCarryJournalAction::CommitState) { return { SyncStoreStatus::InvalidTransition }; }
	const auto hash = action == SyncCarryJournalAction::WritePendingState ? _journal->pendingStateHash : _journal->nextStateHash;
	if (hash.isEmpty()) {
		if (!_stateFile.absent()) { return { SyncStoreStatus::Changed }; }
		const auto barrier = WitnessState(QString()); if (barrier != SyncStoreStatus::Ready) { return Fail(barrier); }
	} else {
		const auto bytes = ReadObject(u"state"_q, hash); if (!bytes) { return { SyncStoreStatus::InvalidState }; }
		const auto state = ParseSyncLocalState(*bytes);
		if (!state) { return { SyncStoreStatus::InvalidState }; }
		if (!state.state.configData.base.isEmpty()) {
			const auto base = FindBase(state.state); if (!base) { return { base.status }; }
			const auto retained = StoreObject(u"record"_q, base.staged); if (!retained) { return retained; }
		}
		const auto status = WriteSyncDurableFile(_directory + u"/state.json"_q, *bytes, _stateFile, _fault);
		if (status != SyncStoreStatus::Ready) { return Fail(status); }
		_stateFile = CaptureSyncPrivateFile(_directory + u"/state.json"_q, kSyncCarryJournalStorageLimit);
		if (_stateFile.status != SyncStoreStatus::Ready || _stateFile.bytes != *bytes) { return Fail(SyncStoreStatus::Changed); }
		_local->_state = ParseSyncLocalState(*bytes).state;
	}
	auto event = SyncCarryEvent(*_journal, SyncCarryEventKind::StateObserved);
	event.outcome = SyncCarryOutcome::Success;
	event.state = _local->_state; event.stateAbsentConfirmed = _stateFile.absent(); event.objectsDurable = true;
	return Apply(event);
}

SyncChatCarryStorageResult SyncChatCarryStore::RetireStage() {
	if (!StateMatches() || _foreignLogin || !_journal) { return { SyncStoreStatus::InvalidTransition }; }
	const auto action = NextSyncCarryJournalAction(*_journal);
	if (action != SyncCarryJournalAction::RetirePendingStage && action != SyncCarryJournalAction::RetireOwnStage) { return { SyncStoreStatus::InvalidTransition }; }
	if (_local->_state && !_local->_state->configData.base.isEmpty()) {
		const auto base = OwnBase(); if (!base) { return { base.status }; }
		const auto retained = StoreObject(u"record"_q, base.staged); if (!retained) { return retained; }
	}
	const auto hash = action == SyncCarryJournalAction::RetirePendingStage ? _journal->pendingStageHash : _journal->plan->ownRecordHash;
	const auto seq = action == SyncCarryJournalAction::RetirePendingStage ? _journal->pendingSeq : ParseSyncEnvelope(_journal->plan->ownRecord).header->seq;
	const auto durable = WitnessState(action == SyncCarryJournalAction::RetirePendingStage ? _journal->pendingStateHash : _journal->nextStateHash);
	if (durable != SyncStoreStatus::Ready) { return Fail(durable); }
	const auto directory = _directory + u"/pending"_q;
	const auto path = directory + u"/config-"_q + QString::number(seq) + u".json"_q;
	const auto staged = CaptureSyncPrivateFile(path, kSyncSettingsRecordStorageLimit);
	if (staged.status != SyncStoreStatus::Ready || (!staged.absent() && SyncCarryRecordHash(staged.bytes) != hash)) { return { SyncStoreStatus::PendingMismatch }; }
	if (!staged.absent() && (!Same(staged, CaptureSyncPrivateFile(path, kSyncSettingsRecordStorageLimit)) || !QFile::remove(path))) { return Fail(SyncStoreStatus::CleanupFailed); }
	if (_fault && !_fault(SyncDurablePoint::DirectoryBarrier)) { return Fail(SyncStoreStatus::DurabilityFailed); }
	const auto barrier = SyncPrivateDirectoryBarrier(directory); if (barrier != SyncStoreStatus::Ready) { return Fail(barrier); }
	auto entries = QDirIterator(directory, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
	if (entries.hasNext()) {
		const auto remaining = entries.next();
		const auto pending = _journal->targetPendingRecord;
		const auto parsed = ParseSyncEnvelope(pending);
		if (action != SyncCarryJournalAction::RetireOwnStage || !parsed.header
			|| remaining != directory + u"/config-"_q + QString::number(parsed.header->seq) + u".json"_q
			|| entries.hasNext()) { return { SyncStoreStatus::OrphanStage }; }
	}
	if (action == SyncCarryJournalAction::RetireOwnStage) {
		const auto pending = InstallTargetPending(); if (pending != SyncStoreStatus::Ready) { return Fail(pending); }
	}
	auto event = SyncCarryEvent(*_journal, SyncCarryEventKind::StageRetired);
	event.outcome = SyncCarryOutcome::Success;
	event.retiredStageHash = hash; event.noPendingFiles = _journal->targetPendingRecord.isEmpty();
	event.state = _local->_state;
	event.targetPendingStageDurable = !_journal->targetPendingRecord.isEmpty();
	event.stagedRecord = _journal->targetPendingRecord; event.objectsDurable = true;
	return Apply(event);
}

SyncChatCarryStorageResult SyncChatCarryStore::Resume() {
	if (!StateMatches() || !_journal || (_foreignLogin && SyncCarryJournalHoldsWrites(*_journal))) { return { SyncStoreStatus::InvalidTransition }; }
	const auto hash = StateHash(_local->_state);
	const auto status = WitnessState(hash);
	if (status != SyncStoreStatus::Ready) { return Fail(status); }
	const auto next = ResumeSyncCarryJournal(*_journal, _local->_state, _stateFile.absent(), true, DesktopSyncCarryLogin(_scope.loginAuthority));
	return !next.canonical.isEmpty() ? Persist(next) : SyncChatCarryStorageResult{ SyncStoreStatus::InvalidTransition, next.status };
}

const SyncCarryJournal *SyncChatCarryStore::journal() const { return _journal ? &*_journal : nullptr; }
const SyncLocalState *SyncChatCarryStore::state() const { return _local && _local->_state ? &*_local->_state : nullptr; }
SyncStoreResult SyncChatCarryStore::FindBase(const SyncLocalState &state) const {
	const auto embedded = ReadSyncRetainedConfigBase(state);
	if (embedded || embedded.status != SyncStoreStatus::MissingLocalVersion) { return embedded; }
	for (auto i = _objects.begin(); i != _objects.end(); ++i) {
		if (i.value().toString() != u"record"_q) { continue; }
		const auto bytes = ReadObject(u"record"_q, i.key());
		if (!bytes) { return { SyncStoreStatus::InvalidRetention }; }
		const auto parsed = ParseSyncEnvelope(*bytes);
		const auto payload = InspectConfigPayload(parsed);
		const auto encoded = parsed ? SerializeSyncEnvelope(parsed.envelope) : SyncEnvelopeWriteResult();
		if (encoded && encoded.canonical == *bytes && payload.status == ConfigPayloadStatus::Valid
			&& parsed.envelope.document.value(u"space"_q).toString() == state.space
			&& payload.version.key == state.configData.base
			&& payload.version.lineage == state.configData.baseLineage) {
			return { SyncStoreStatus::Ready, SyncLocalError::None, SyncEnvelopeError::None,
				ConfigPayloadError::None, *bytes };
		}
	}
	return { SyncStoreStatus::MissingLocalVersion };
}

SyncStoreResult SyncChatCarryStore::OwnBase() const {
	return Current() && state() ? FindBase(*state()) : SyncStoreResult{ SyncStoreStatus::MissingLocalVersion };
}

SyncStoreStatus SyncChatCarryStore::InstallTargetPending(bool interrupted) {
	if (!StateMatches() || !_journal) { return SyncStoreStatus::Changed; }
	const auto &bytes = _journal->targetPendingRecord;
	if (bytes.isEmpty()) { return SyncStoreStatus::Ready; }
	if (!_local->_state || (interrupted
		? !_foreignLogin || !_journal->commitPrepared || StateHash(_local->_state) != _journal->nextStateHash
		: !_journal->stateCommitted)) { return SyncStoreStatus::InvalidTransition; }
	const auto parsed = ParseSyncEnvelope(bytes);
	if (!parsed.header || _local->_state->config.pendingSeq != parsed.header->seq
		|| _local->_state->configData.pending != InspectConfigPayload(parsed).version.key) { return SyncStoreStatus::PendingMismatch; }
	const auto directory = _directory + u"/pending"_q;
	const auto path = directory + u"/config-"_q + QString::number(parsed.header->seq) + u".json"_q;
	const auto snapshot = CaptureSyncPrivateFile(path, kSyncSettingsRecordStorageLimit);
	if (snapshot.status != SyncStoreStatus::Ready || (!snapshot.absent() && snapshot.bytes != bytes)) { return SyncStoreStatus::PendingMismatch; }
	return snapshot.absent() ? WriteSyncDurableFile(path, bytes, snapshot, _fault)
		: ConfirmSyncDurableFile(path, snapshot, _fault);
}

SyncStoreStatus SyncChatCarryStore::RestoreInterruptedPending() {
	if (!StateMatches() || !_foreignLogin || !_journal || !_local->_state
		|| _journal->targetPendingRecord.isEmpty() || !_journal->commitPrepared
		|| StateHash(_local->_state) != _journal->nextStateHash) {
		return SyncStoreStatus::InvalidTransition;
	}
	const auto &bytes = _journal->targetPendingRecord;
	const auto retained = ReadObject(u"record"_q, SyncCarryRecordHash(bytes));
	const auto parsed = ParseSyncEnvelope(bytes);
	const auto preview = AbandonInterruptedOperation(*_journal, _scope.account,
		DesktopSyncCarryLogin(_scope.loginAuthority), true, _local->_state,
		_stateFile.absent(), bytes, true);
	if (!retained || *retained != bytes || !parsed.header || !preview) {
		return SyncStoreStatus::PendingMismatch;
	}
	const auto directory = _directory + u"/pending"_q;
	const auto target = directory + u"/config-"_q + QString::number(parsed.header->seq) + u".json"_q;
	auto obsolete = QString();
	auto oldBytes = QByteArray();
	if (_journal->plan && !_journal->plan->ownRecord.isEmpty()) {
		oldBytes = _journal->plan->ownRecord;
		const auto old = ParseSyncEnvelope(oldBytes);
		const auto object = ReadObject(u"record"_q, _journal->plan->ownRecordHash);
		if (!old.header || !object || *object != oldBytes
			|| SyncCarryRecordHash(oldBytes) != _journal->plan->ownRecordHash) {
			return SyncStoreStatus::PendingMismatch;
		}
		obsolete = directory + u"/config-"_q + QString::number(old.header->seq) + u".json"_q;
		if (obsolete == target) { return SyncStoreStatus::PendingMismatch; }
	}
	auto entries = QDirIterator(directory, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
	while (entries.hasNext()) {
		const auto path = entries.next();
		if (path != target && path != obsolete) { return SyncStoreStatus::OrphanStage; }
	}
	const auto old = obsolete.isEmpty() ? SyncPrivateSnapshot()
		: CaptureSyncPrivateFile(obsolete, kSyncSettingsRecordStorageLimit);
	if (!obsolete.isEmpty() && (old.status != SyncStoreStatus::Ready
		|| (!old.absent() && old.bytes != oldBytes))) { return SyncStoreStatus::PendingMismatch; }
	const auto installed = InstallTargetPending(true);
	if (installed != SyncStoreStatus::Ready) { return installed; }
	if (!obsolete.isEmpty() && !old.absent()
		&& (!Same(old, CaptureSyncPrivateFile(obsolete, kSyncSettingsRecordStorageLimit))
			|| !QFile::remove(obsolete))) { return SyncStoreStatus::CleanupFailed; }
	if (_fault && !_fault(SyncDurablePoint::DirectoryBarrier)) { return SyncStoreStatus::DurabilityFailed; }
	return SyncPrivateDirectoryBarrier(directory);
}

SyncStoreResult SyncChatCarryStore::Pending() const {
	if (!Current() || !_local) { return { SyncStoreStatus::WrongThread }; }
	const auto pending = _local->ReadPendingConfigLocked();
	if (pending && _journal && _journal->phase == SyncCarryPhase::ResolvePending) {
		const auto hash = _envelope.value(u"opening_pending_sha256"_q).toString();
		const auto retained = ReadObject(u"record"_q, hash);
		if (!retained || *retained != pending.staged) { return { SyncStoreStatus::PendingMismatch }; }
	}
	return pending;
}

bool SyncChatCarryStore::foreignLogin() const { return _foreignLogin; }

SyncChatCarryStorageResult SyncChatCarryStore::DiscardInterrupted(bool confirmed) {
	if (!confirmed || !StateMatches() || !_journal || !_foreignLogin) { return { SyncStoreStatus::InvalidTransition }; }
	const auto witnessed = WitnessState(StateHash(_local->_state));
	if (witnessed != SyncStoreStatus::Ready) { return Fail(witnessed); }
	auto stage = QByteArray();
	if (_local->_state && !_journal->targetPendingRecord.isEmpty()
		&& _journal->commitPrepared && StateHash(_local->_state) == _journal->nextStateHash) {
		const auto restored = RestoreInterruptedPending();
		if (restored != SyncStoreStatus::Ready) { return Fail(restored); }
	}
	if (_local->_state && _local->_state->config.pendingSeq) {
		const auto pending = Pending(); if (!pending) { return { pending.status }; } stage = pending.staged;
		const auto path = _directory + u"/pending/config-"_q + QString::number(_local->_state->config.pendingSeq) + u".json"_q;
		const auto snapshot = CaptureSyncPrivateFile(path, kSyncSettingsRecordStorageLimit);
		if (snapshot.status != SyncStoreStatus::Ready || snapshot.bytes != stage) { return { SyncStoreStatus::PendingMismatch }; }
		const auto durable = ConfirmSyncDurableFile(path, snapshot, _fault);
		if (durable != SyncStoreStatus::Ready) { return Fail(durable); }
	}
	const auto expectedStage = _local->_state && _local->_state->config.pendingSeq
		? _directory + u"/pending/config-"_q + QString::number(_local->_state->config.pendingSeq) + u".json"_q : QString();
	auto stages = QDirIterator(_directory + u"/pending"_q,
		QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
	while (stages.hasNext()) {
		if (stages.next() != expectedStage) { return {SyncStoreStatus::OrphanStage}; }
	}
	const auto next = AbandonInterruptedOperation(*_journal, _scope.account,
		DesktopSyncCarryLogin(_scope.loginAuthority), confirmed, _local->_state, _stateFile.absent(), stage, true);
	if (next.canonical.isEmpty()) { return { SyncStoreStatus::InvalidTransition, next.status }; }
	return Persist(next);
}

SyncChatCarryStorageResult SyncChatCarryStore::WriteCache(const DesktopSyncChatPeer &peer, bool complete) {
	if (!StateMatches() || !_journal || _foreignLogin || !_journal->targetMark
		|| peer.origin.account != _scope.account || peer.chat != _journal->target
		|| NextSyncCarryJournalAction(*_journal) != (complete ? SyncCarryJournalAction::CompleteCache : SyncCarryJournalAction::PrepareCache)) {
		return { SyncStoreStatus::InvalidTransition };
	}
	const auto eligible = DesktopSyncChatEligibility(peer);
	if (!eligible || CheckSyncCarryReaders(*_journal, *eligible) == SyncCarryReaderVerdict::Hold) { return { SyncStoreStatus::InvalidTransition }; }
	const auto authority = DesktopSyncCarryTransportOf(_envelope, *_journal);
	if (!authority || !DesktopSyncCarryReaderAuthority(authority->value(u"readers"_q).toObject(), peer)) { return { SyncStoreStatus::InvalidTransition }; }
	const auto path = complete ? _scope.cacheFile : _directory + u"/carry-cache.json"_q;
	auto expected = CaptureSyncPrivateFile(path, 64 * 1024);
	if (expected.status != SyncStoreStatus::Ready || (complete && !Same(expected, _cacheFile))) { return { SyncStoreStatus::Changed }; }
	if (!complete && !expected.absent()) {
		const auto prepared = ParseDesktopSyncChatCache(expected.bytes, _scope.account);
		const auto retained = RetainedEffects();
		if (!prepared.record || !prepared.record->carry || !retained) { return { SyncStoreStatus::InvalidState }; }
		const auto operation = prepared.record->carry->value(u"operation"_q).toString();
		if (std::none_of(retained->begin(), retained->end(), [&](const auto &journal) { return journal.operationId == operation; })) { return { SyncStoreStatus::InvalidState }; }
	}
	auto record = DesktopSyncChatCacheRecord();
	if (!_cacheFile.absent()) {
		const auto previous = ParseDesktopSyncChatCache(_cacheFile.bytes, _scope.account);
		if (previous.status != DesktopSyncChatCacheStatus::Valid || previous.paused
			|| (previous.record && (previous.record->loginBinding != _scope.loginAuthority
				|| (previous.record->mark != *_journal->targetMark && (!_journal->oldMark || previous.record->mark != *_journal->oldMark))))) { return { SyncStoreStatus::InvalidState }; }
		if (previous.record) { record = *previous.record; }
		if (record.mark.chat != _journal->target) {
			record.readerMembers.reset(); record.readerCreator.reset();
			record.disclosedStreams.clear(); record.newReaders = false;
		}
	}
	if (complete) {
		const auto prepared = CaptureSyncPrivateFile(_directory + u"/carry-cache.json"_q, 64 * 1024);
		const auto parsed = ParseDesktopSyncChatCache(prepared.bytes, _scope.account);
		if (prepared.status != SyncStoreStatus::Ready || prepared.absent() || !parsed.record
			|| !parsed.record->carry || parsed.record->carry->value(u"operation"_q).toString() != _journal->operationId
			|| parsed.record->mark != *_journal->targetMark || parsed.record->loginBinding != _scope.loginAuthority) { return { SyncStoreStatus::InvalidState }; }
		const auto warning = record.newReaders;
		const auto streams = record.disclosedStreams;
		record = *parsed.record;
		record.newReaders = record.newReaders || warning;
		for (const auto &stream : streams) {
			if (std::find(record.disclosedStreams.begin(), record.disclosedStreams.end(), stream) == record.disclosedStreams.end()) { record.disclosedStreams.push_back(stream); }
		}
		record.carry.reset();
	} else {
		record.account = _scope.account; record.mark = *_journal->targetMark;
		record.readers = *_journal->readerBaseline;
		const auto transport = _envelope.value(u"transport"_q).toObject();
		const auto baseline = transport.value(u"readers"_q).toObject();
		if (baseline.value(u"members"_q).isArray() && baseline.value(u"members_known"_q) == QJsonValue(true)) {
			auto members = std::vector<QString>();
			for (const auto &member : baseline.value(u"members"_q).toArray()) {
				if (!member.isString()) { return { SyncStoreStatus::InvalidState }; } members.push_back(member.toString());
			}
			if (!record.readerMembers) { record.readerMembers = std::move(members); }
		}
		if (baseline.value(u"creator"_q).isBool() && !record.readerCreator) { record.readerCreator = baseline.value(u"creator"_q).toBool(); }
		record.newReaders = record.newReaders || transport.value(u"reader_warning"_q).toBool()
			|| transport.value(u"pending_reader_warning"_q).toBool();
		const auto stream = baseline.value(u"stream"_q).toString();
		if ((stream != u"config"_q && stream != u"library"_q) || !baseline.value(u"settings_disclosed"_q).toBool()) { return { SyncStoreStatus::InvalidState }; }
		if (std::find(record.disclosedStreams.begin(), record.disclosedStreams.end(), stream) == record.disclosedStreams.end()) { record.disclosedStreams.push_back(stream); }
		if (std::find(record.disclosedStreams.begin(), record.disclosedStreams.end(), u"config"_q) == record.disclosedStreams.end()) { record.disclosedStreams.push_back(u"config"_q); }
		record.readerConsent = true;
		record.loginBinding = _scope.loginAuthority;
		record.carry = QJsonObject{{u"operation"_q, _journal->operationId}};
	}
	record.newReaders = record.newReaders
		|| CheckSyncCarryReaders(*_journal, *eligible) == SyncCarryReaderVerdict::WarnAndProceed;
	const auto bytes = SerializeDesktopSyncChatCache(record);
	if (!bytes) { return { SyncStoreStatus::InvalidState }; }
	const auto status = WriteSyncDurableFile(path, *bytes, expected, _fault);
	if (status != SyncStoreStatus::Ready) { return Fail(status); }
	if (complete) { _cacheFile = CaptureSyncPrivateFile(path, 64 * 1024); }
	auto event = SyncCarryEvent(*_journal, complete ? SyncCarryEventKind::CacheCompleted : SyncCarryEventKind::CachePrepared);
	event.outcome = SyncCarryOutcome::Success;
	event.cache = {_journal->operationId, _scope.account, *_journal->targetMark, complete};
	return Advance(event);
}

SyncChatCarryStorageResult SyncChatCarryStore::ArchiveTerminal(bool clear) {
	if (!StateMatches() || !_journal || SyncCarryJournalHoldsWrites(*_journal)) { return { SyncStoreStatus::InvalidTransition }; }
	const auto directory = _directory + u"/carry-history"_q;
	const auto created = EnsureSyncPrivateDirectory(directory);
	if (created != SyncStoreStatus::Ready) { return { created }; }
	const auto barrier = SyncPrivateDirectoryBarrier(_directory);
	if (barrier != SyncStoreStatus::Ready) { return Fail(barrier); }
	const auto bytes = _journalFile.absent() ? CanonicalizeSyncJson(QJsonDocument(_envelope).toJson(QJsonDocument::Compact)).canonical : _journalFile.bytes;
	if (bytes.isEmpty()) { return {SyncStoreStatus::InvalidState}; }
	auto count = 0;
	auto size = qint64(0);
	auto entries = QDirIterator(directory, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
	while (entries.hasNext()) { entries.next(); ++count; size += entries.fileInfo().size(); }
	const auto path = directory + u'/' + SyncCarryRecordHash(bytes) + u".json"_q;
	const auto snapshot = CaptureSyncPrivateFile(path, kSyncCarryJournalStorageLimit);
	if (snapshot.status != SyncStoreStatus::Ready || (!snapshot.absent() && snapshot.bytes != bytes)) { return { SyncStoreStatus::Changed }; }
	if (snapshot.absent() && (count >= 64 || size + bytes.size() > kSyncCarryObjectStorageLimit)) { return {SyncStoreStatus::RetentionFull}; }
	const auto written = snapshot.absent() ? WriteSyncDurableFile(path, bytes, snapshot, _fault)
		: ConfirmSyncDurableFile(path, snapshot, _fault);
	if (written != SyncStoreStatus::Ready) { return Fail(written); }
	if (!_journalFile.absent()) {
		const auto active = _directory + u"/carry.json"_q;
		if (!Same(_journalFile, CaptureSyncPrivateFile(active, kSyncCarryJournalStorageLimit)) || !QFile::remove(active)) { return Fail(SyncStoreStatus::CleanupFailed); }
		if ((_fault && !_fault(SyncDurablePoint::DirectoryBarrier)) || SyncPrivateDirectoryBarrier(_directory) != SyncStoreStatus::Ready) { return Fail(SyncStoreStatus::DurabilityFailed); }
		_journalFile = CaptureSyncPrivateFile(active, kSyncCarryJournalStorageLimit);
		if (!_journalFile.absent()) { return Fail(SyncStoreStatus::Changed); }
	}
	if (clear) { _journal.reset(); _envelope = {}; _foreignLogin = false; }
	return { SyncStoreStatus::Ready };
}

std::optional<std::vector<QByteArray>> SyncChatCarryStore::RetainedImages() const {
	if (!Current()) { return {}; }
	auto result = std::vector<QByteArray>();
	auto operations = std::map<QString, QByteArray>();
	auto total = qint64(0);
	const auto binding = _scope.binding.isEmpty() && _foreignLogin && _local->_state
		? _local->_state->bindingToken.toLatin1() : _scope.binding;
	const auto append = [&](const QByteArray &bytes) {
		const auto journal = InspectSyncCarryJournal(bytes, binding);
		if (!journal || journal->account != _scope.account) { return false; }
		const auto existing = operations.find(journal->operationId);
		if (existing != operations.end()) { return existing->second == bytes; }
		total += bytes.size();
		if (operations.size() >= 65 || total > kSyncCarryObjectStorageLimit) { return false; }
		operations.emplace(journal->operationId, bytes); result.push_back(bytes); return true;
	};
	if (_journal) {
		const auto bytes = CanonicalizeSyncJson(QJsonDocument(_envelope).toJson(QJsonDocument::Compact));
		if (!bytes || !append(bytes.canonical)) { return {}; }
	}
	const auto directory = _directory + u"/carry-history"_q;
	if (SyncPathIdentity(directory) != "missing" && (QFileInfo(directory).isSymLink() || !QFileInfo(directory).isDir())) { return {}; }
	auto entries = QDirIterator(directory, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
	while (entries.hasNext()) {
		const auto path = entries.next();
		const auto snapshot = CaptureSyncPrivateFile(path, kSyncCarryJournalStorageLimit);
		if (snapshot.status != SyncStoreStatus::Ready || snapshot.absent()
			|| QFileInfo(path).fileName() != SyncCarryRecordHash(snapshot.bytes) + u".json"_q || !append(snapshot.bytes)) { return {}; }
	}
	return result;
}

std::optional<std::vector<SyncCarryJournal>> SyncChatCarryStore::RetainedEffects() const {
	const auto images = RetainedImages();
	if (!images) { return {}; }
	auto result = std::vector<SyncCarryJournal>();
	const auto binding = _scope.binding.isEmpty() && _foreignLogin && _local->_state
		? _local->_state->bindingToken.toLatin1() : _scope.binding;
	for (const auto &bytes : *images) {
		const auto journal = InspectSyncCarryJournal(bytes, binding);
		if (!journal) { return {}; }
		result.push_back(*journal);
	}
	return result;
}

std::optional<SyncCarryJournal> InspectSyncCarryJournal(const QByteArray &bytes, const QByteArray &bindingToken) {
	const auto canonical = CanonicalizeSyncJson(bytes);
	if (!canonical) { return {}; }
	const auto envelope = QJsonDocument::fromJson(canonical.canonical).object();
	if (envelope.value(u"version"_q).toDouble() != 1
		|| !envelope.value(u"journal"_q).isObject()) { return {}; }
	const auto journal = ParseSyncCarryJournal(QJsonDocument(envelope.value(u"journal"_q).toObject()).toJson(QJsonDocument::Compact));
	if (!journal || journal.journal.version != 2
		|| !Hash(envelope.value(u"login_authority"_q).toString())
		|| journal.journal.loginAuthority != DesktopSyncCarryLogin(envelope.value(u"login_authority"_q).toString())
		|| !envelope.value(u"objects"_q).isObject()
		|| (envelope.value(u"binding_sha256"_q).toString() != SyncCarryRecordHash(bindingToken)
		&& !(envelope.value(u"binding_sha256"_q).toString() == SyncCarryRecordHash({})
			&& journal.journal.mode == SyncCarryMode::Fill && journal.journal.stateHash.isEmpty()
			&& !SyncCarryJournalHoldsWrites(journal.journal)))
		|| envelope.value(u"journal_sha256"_q).toString() != SyncCarryRecordHash(journal.canonical)
		|| envelope.value(u"operation"_q).toString() != journal.journal.operationId
		|| envelope.value(u"account"_q).toString() != AccountKeyText(journal.journal.account.environment, journal.journal.account.userId)) { return {}; }
	const auto pending = envelope.value(u"opening_pending_sha256"_q);
	if ((!pending.isUndefined() && (!pending.isString() || !Hash(pending.toString())
		|| envelope.value(u"objects"_q).toObject().value(pending.toString()).toString() != u"record"_q))
		|| (journal.journal.phase == SyncCarryPhase::ResolvePending && pending.isUndefined())) { return {}; }
	return journal.journal;
}

SyncChatCarryStorageResult SyncChatCarryStore::RemoveAccount(bool explicitlyConfirmed) {
	if (!SyncStoreWorkerThread()) { return { SyncStoreStatus::WrongThread }; }
	if (!explicitlyConfirmed) { return { SyncStoreStatus::InvalidTransition }; }
	if (_local || _thread) { return { SyncStoreStatus::InvalidTransition }; }
	const auto opened = Open();
	if (!opened) { return opened; }
	auto files = std::vector<std::pair<QString, SyncPrivateSnapshot>>();
	for (const auto &name : {u"carry.json"_q, u"carry-cache.json"_q}) {
		const auto path = _directory + u'/' + name;
		const auto snapshot = CaptureSyncPrivateFile(path, kSyncCarryJournalStorageLimit);
		if (snapshot.status != SyncStoreStatus::Ready) { return Fail(snapshot.status); }
		if (!snapshot.absent()) { files.emplace_back(path, snapshot); }
	}
	for (const auto &folder : {u"carry-history"_q, u"carry-objects"_q}) {
		const auto directory = _directory + u'/' + folder;
		const auto identity = SyncPathIdentity(directory);
		if (identity == "missing") { continue; }
		if (identity.isEmpty() || QFileInfo(directory).isSymLink() || !QFileInfo(directory).isDir()) { return Fail(SyncStoreStatus::InsecurePermissions); }
		auto entries = QDirIterator(directory, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
		while (entries.hasNext()) {
			const auto path = entries.next();
			const auto name = QFileInfo(path).fileName();
			const auto snapshot = CaptureSyncPrivateFile(path, kSyncCarryJournalStorageLimit);
			if (snapshot.status != SyncStoreStatus::Ready || snapshot.absent() || !name.endsWith(u".json"_q)) { return Fail(SyncStoreStatus::InvalidState); }
			const auto stem = name.left(name.size() - 5);
			if (folder == u"carry-history"_q) {
				const auto journal = InspectSyncCarryJournal(snapshot.bytes, _scope.binding.isEmpty() && _foreignLogin && _local->_state ? _local->_state->bindingToken.toLatin1() : _scope.binding);
				if (!journal || journal->account != _scope.account || stem != SyncCarryRecordHash(snapshot.bytes)) { return Fail(SyncStoreStatus::AccountUnbound); }
			} else {
				const auto separator = stem.indexOf(u'-');
				const auto kind = stem.left(separator);
				if (separator <= 0 || (kind != u"state"_q && kind != u"record"_q && kind != u"text"_q)
					|| stem.mid(separator + 1) != SyncCarryRecordHash(snapshot.bytes)) { return Fail(SyncStoreStatus::InvalidState); }
			}
			files.emplace_back(path, snapshot);
			if (files.size() > 1091) { return Fail(SyncStoreStatus::RetentionFull); }
		}
	}
	for (const auto &[path, snapshot] : files) {
		if (!StateMatches() || !Same(snapshot, CaptureSyncPrivateFile(path, kSyncCarryJournalStorageLimit))) { return Fail(SyncStoreStatus::Changed); }
	}
	for (const auto &[path, snapshot] : files) {
		if (!Same(snapshot, CaptureSyncPrivateFile(path, kSyncCarryJournalStorageLimit)) || !QFile::remove(path)
			|| SyncPrivateDirectoryBarrier(QFileInfo(path).absolutePath()) != SyncStoreStatus::Ready) { return Fail(SyncStoreStatus::CleanupFailed); }
	}
	Close();
	return { SyncStoreStatus::Ready };
}

}

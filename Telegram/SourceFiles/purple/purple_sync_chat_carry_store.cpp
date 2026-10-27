#include "purple/purple_sync_chat_carry_store.h"
#include "purple/purple_sync_chat_cache.h"
#include "purple/purple_sync_json.h"

#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>

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

}

SyncChatCarryStore::SyncChatCarryStore(QString directory, SyncChatCarryStoreScope scope,
		SyncDurableFault fault)
: _directory(std::move(directory))
, _scope(std::move(scope))
, _fault(std::move(fault)) {
}

SyncChatCarryStore::~SyncChatCarryStore() = default;

void SyncChatCarryStore::Close() {
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
	_failed = false; _objects = {}; _envelope = {}; _journal.reset(); _objectBytes = 0;
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
	if (_local->_state && CheckSyncAccountBinding(*_local->_state, _scope.binding) != SyncAccountBindingVerdict::Bound) {
		return Fail(SyncStoreStatus::AccountUnbound);
	}
	const auto path = _directory + u"/carry.json"_q;
	_journalFile = CaptureSyncPrivateFile(path, kSyncCarryJournalStorageLimit);
	if (_journalFile.status != SyncStoreStatus::Ready) { return Fail(_journalFile.status); }
	if (_journalFile.absent()) { return { SyncStoreStatus::Ready }; }
	const auto canonical = CanonicalizeSyncJson(_journalFile.bytes);
	if (!canonical) { return Fail(SyncStoreStatus::InvalidState); }
	_envelope = QJsonDocument::fromJson(canonical.canonical).object();
	const auto version = _envelope.value(u"version"_q);
	if (version.isDouble() && version.toDouble() > 1) { return Fail(SyncStoreStatus::NewerState); }
	if (!version.isDouble() || version.toDouble() != 1) { return Fail(SyncStoreStatus::InvalidState); }
	const auto account = AccountKeyText(_scope.account.environment, _scope.account.userId);
	if (_envelope.value(u"account"_q).toString() != *account
		|| _envelope.value(u"binding_sha256"_q).toString() != BindingHash(_scope.binding)
		|| _envelope.value(u"login_authority"_q).toString() != _scope.loginAuthority) {
		return Fail(SyncStoreStatus::AccountUnbound);
	}
	if (!_envelope.value(u"journal"_q).isObject() || !_envelope.value(u"objects"_q).isObject()) {
		return Fail(SyncStoreStatus::InvalidState);
	}
	const auto journal = ParseSyncCarryJournal(QJsonDocument(_envelope.value(u"journal"_q).toObject()).toJson());
	if (!journal || journal.journal.account != _scope.account
		|| _envelope.value(u"operation"_q).toString() != journal.journal.operationId
		|| _envelope.value(u"journal_sha256"_q).toString() != SyncCarryRecordHash(journal.canonical)) {
		return Fail(journal.status == SyncCarryJournalStatus::NewerVersion
			? SyncStoreStatus::NewerState : SyncStoreStatus::InvalidState);
	}
	_objects = _envelope.value(u"objects"_q).toObject();
	if (_objects.size() > 1024) { return Fail(SyncStoreStatus::RetentionFull); }
	for (auto i = _objects.begin(); i != _objects.end(); ++i) {
		const auto kind = i.value().toString();
		const auto bytes = ReadObject(kind, i.key());
		if (!bytes) { return Fail(SyncStoreStatus::InvalidState); }
		_objectBytes += bytes->size();
		if (_objectBytes > kSyncCarryObjectStorageLimit) { return Fail(SyncStoreStatus::RetentionFull); }
	}
	_journal = journal.journal;
	return { SyncStoreStatus::Ready, journal.status };
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
	document.insert(u"binding_sha256"_q, BindingHash(_scope.binding));
	document.insert(u"login_authority"_q, _scope.loginAuthority);
	document.insert(u"operation"_q, next.journal.operationId);
	document.insert(u"journal_sha256"_q, SyncCarryRecordHash(next.canonical));
	document.insert(u"journal"_q, QJsonDocument::fromJson(next.canonical).object());
	document.insert(u"objects"_q, _objects);
	const auto encoded = CanonicalizeSyncJson(QJsonDocument(document).toJson());
	if (!encoded || encoded.canonical.size() > kSyncCarryJournalStorageLimit) { return { SyncStoreStatus::RetentionFull }; }
	const auto path = _directory + u"/carry.json"_q;
	const auto status = WriteSyncDurableFile(path, encoded.canonical, _journalFile, _fault);
	if (status != SyncStoreStatus::Ready) { return Fail(status); }
	_journalFile = CaptureSyncPrivateFile(path, kSyncCarryJournalStorageLimit);
	if (_journalFile.status != SyncStoreStatus::Ready || _journalFile.bytes != encoded.canonical
		|| _journalFile.parentIdentity != _directoryIdentity) { return Fail(SyncStoreStatus::Changed); }
	_envelope = document; _journal = next.journal;
	return { SyncStoreStatus::Ready, next.status };
}

SyncChatCarryStorageResult SyncChatCarryStore::Begin(const SyncCarryJournalStart &input) {
	if (!StateMatches() || _journal || !_journalFile.absent() || input.account != _scope.account
		|| input.preferenceToken != _scope.binding || StateHash(input.state) != StateHash(_local->_state)
		|| (!input.state && (!input.stateAbsentConfirmed || !_stateFile.absent()))) {
		return { SyncStoreStatus::InvalidTransition };
	}
	auto start = input;
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
	}
	const auto next = BeginSyncCarryJournal(start);
	if (!next) { return { SyncStoreStatus::InvalidTransition, next.status }; }
	if (_local->_state) {
		if (!_local->_state->configData.base.isEmpty() && !OwnBase()) { return { SyncStoreStatus::MissingLocalVersion }; }
		if (!_local->_state->configData.base.isEmpty()) {
			const auto record = StoreObject(u"record"_q, OwnBase().staged); if (!record) { return record; }
		}
		const auto prepared = PrepareState(*_local->_state); if (!prepared) { return prepared; }
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
	const auto next = AdvanceSyncCarryJournal(*_journal, event);
	return !next.canonical.isEmpty() ? Persist(next) : SyncChatCarryStorageResult{ SyncStoreStatus::InvalidTransition, next.status };
}

SyncChatCarryStorageResult SyncChatCarryStore::Advance(SyncCarryJournalEvent event) {
	if (!StateMatches() || !_journal || event.kind == SyncCarryEventKind::StateObserved
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
		const auto pending = event.pendingReadback
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
		const auto commit = PrepareSyncCarryCommit(*_journal->plan, _local->_state, *event.destination, event.playlist);
		if (!commit) { return Apply(event); }
		if (commit.state) { const auto image = PrepareState(*commit.state); if (!image) { return image; } }
		event.preparedImageHash = commit.stateHash; event.objectsDurable = true;
	} else if (event.kind == SyncCarryEventKind::CachePrepared || event.kind == SyncCarryEventKind::CacheCompleted) {
		const auto status = WitnessCache(event.cache);
		if (status != SyncStoreStatus::Ready) { return Fail(status); }
		event.objectsDurable = true;
	}
	return Apply(event);
}

SyncChatCarryStorageResult SyncChatCarryStore::StageOwn() {
	if (!StateMatches() || !_journal || !_journal->plan || !_local->_state
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
	if (!StateMatches() || !_journal) { return { SyncStoreStatus::InvalidTransition }; }
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
	if (!StateMatches() || !_journal) { return { SyncStoreStatus::InvalidTransition }; }
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
	if (entries.hasNext()) { return { SyncStoreStatus::OrphanStage }; }
	auto event = SyncCarryEvent(*_journal, SyncCarryEventKind::StageRetired);
	event.outcome = SyncCarryOutcome::Success;
	event.retiredStageHash = hash; event.noPendingFiles = true;
	return Apply(event);
}

SyncChatCarryStorageResult SyncChatCarryStore::Resume() {
	if (!StateMatches() || !_journal) { return { SyncStoreStatus::InvalidTransition }; }
	const auto hash = StateHash(_local->_state);
	const auto status = WitnessState(hash);
	if (status != SyncStoreStatus::Ready) { return Fail(status); }
	const auto next = ResumeSyncCarryJournal(*_journal, _local->_state, _stateFile.absent(), true);
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

}

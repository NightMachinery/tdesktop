#include "purple/purple_sync_auto_store.h"
#include "purple/purple_sync_config_review.h"

#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>

#include <limits>
#include <algorithm>

namespace Purple {
namespace {

QString AutoPath(const AccountFiles &files) {
	return files.syncDirectory + u"/auto.json"_q;
}

QByteArray StateBytes(const SyncLocalState &state) {
	return SerializeSyncLocalState(state).canonical;
}

SyncAutoPlan Refused(const SyncAutoRecord &record) {
	auto result = SyncAutoPlan();
	result.action = SyncAutoAction::Hold;
	result.reason = SyncAutoReason::StoreMismatch;
	result.record = record;
	return result;
}

}

DesktopSyncAutoRead ReadDesktopSyncAuto(
		const AccountFiles &files, const AccountKey &account) {
	auto result = DesktopSyncAutoRead();
	const auto path = AutoPath(files);
	const auto identity = SyncPathIdentity(path);
	if (identity.isEmpty()) { return result; }
	if (identity == "missing") {
		auto record = SyncAutoRecord();
		record.account = account;
		result.record = record;
		result.status = SyncStoreStatus::Ready;
		return result;
	}
	result.status = ReadSyncPrivateFile(path, kSyncAutoRecordLimit, result.bytes);
	if (result.status != SyncStoreStatus::Ready) { return result; }
	const auto parsed = ParseSyncAutoRecord(result.bytes);
	if (parsed.status != SyncAutoCodecStatus::Valid || !parsed.record
		|| parsed.record->account != account) {
		result.status = SyncStoreStatus::InvalidState;
		return result;
	}
	result.record = parsed.record;
	return result;
}

bool DesktopSyncAutoEditAuthorized(const SyncLocalState &state, const SyncAutoServerRead &read) {
	if (!read.fresh || !read.owned || read.presence != SyncAutoMessagePresence::Present) { return false; }
	const auto found = std::find_if(state.ownMessages.begin(), state.ownMessages.end(), [&](const auto &entry) {
		return entry.messageId == read.messageId && entry.seq == state.config.confirmedSeq;
	});
	const auto adopted = AdoptIssuedOwnConfigMessage(state, read.messageId, read.record);
	return found != state.ownMessages.end() && adopted && StateBytes(adopted.state) == StateBytes(state);
}

bool SameDesktopSyncAutoContext(
		const AccountSettingsOpenTarget &a,
		const AccountSettingsOpenTarget &b) {
	return a.requester == b.requester && a.role == b.role && a.owner == b.owner && a.global == b.global
		&& a.file.account == b.file.account && a.file.path == b.file.path
		&& a.root == b.root && a.rootIdentity == b.rootIdentity
		&& a.accountsIdentity == b.accountsIdentity
		&& a.accountDirectoryIdentity == b.accountDirectoryIdentity
		&& a.localInput == b.localInput && a.ownerInput == b.ownerInput;
}

DesktopSyncAutoStore::DesktopSyncAutoStore(
		AccountFiles files, AccountKey account, QByteArray token,
		std::function<bool()> current)
: _files(std::move(files))
, _account(account)
, _token(std::move(token))
, _current(std::move(current))
, _local(_files.syncDirectory) {
	_record.account = account;
}

bool DesktopSyncAutoStore::Open() {
	if (_opened || !_current || !_current()) { return false; }
	const auto snapshot = ReadSyncRecoveryStore(_files, _account);
	if (snapshot.status != SyncStoreStatus::Ready || !snapshot.store.statePresent) { return false; }
	const auto state = ParseSyncLocalState(snapshot.store.state);
	if (!state || CheckSyncAccountBinding(state.state, _token) != SyncAccountBindingVerdict::Bound) { return false; }
	_directoryIdentity = SyncPathIdentity(_files.syncDirectory);
	if (_directoryIdentity.isEmpty() || _directoryIdentity == "missing" || !_local.Open(true, _token)) { return false; }
	if (!_local.state() || StateBytes(*_local.state()) != StateBytes(state.state)) { return false; }
	const auto read = ReadDesktopSyncAuto(_files, _account);
	if (!read.record || read.status != SyncStoreStatus::Ready) { return false; }
	_record = *read.record;
	_recordBytes = read.bytes;
	_opened = true;
	return Current();
}

bool DesktopSyncAutoStore::RecordCurrent() const {
	if (!_opened || SyncPathIdentity(_files.syncDirectory) != _directoryIdentity) { return false; }
	const auto record = ReadDesktopSyncAuto(_files, _account);
	return record.status == SyncStoreStatus::Ready && record.record && record.bytes == _recordBytes;
}

bool DesktopSyncAutoStore::Current() const {
	if (!RecordCurrent() || !_current || !_current() || !_local.state()) { return false; }
	auto state = QByteArray();
	return ReadSyncPrivateFile(_files.syncStore, 4 * 1024 * 1024, state) == SyncStoreStatus::Ready
		&& state == StateBytes(*_local.state());
}

DesktopSyncAutoCompletion DesktopSyncAutoStore::CheckCompletion(
		const SyncAutoPlan &plan, const QString &owner) const {
	if (!_opened || !_record.active || _record.ticket != plan.ticket
		|| _record.session != owner || plan.record.session != owner) {
		return DesktopSyncAutoCompletion::Obsolete;
	}
	if (!_current || !_current()) { return DesktopSyncAutoCompletion::ContextChanged; }
	return _local.status() == SyncStoreStatus::Ready && _local.state() && Current()
		? DesktopSyncAutoCompletion::Ready : DesktopSyncAutoCompletion::LocalFailure;
}

bool DesktopSyncAutoStore::WriteRecord(const SyncAutoRecord &record, bool requireCurrent) {
	const auto current = [&] { return requireCurrent ? Current() : RecordCurrent(); };
	const auto bytes = SerializeSyncAutoRecord(record);
	if (!bytes || !current()) { return false; }
	if (*bytes != _recordBytes) {
		auto file = QSaveFile(AutoPath(_files));
		file.setDirectWriteFallback(false);
		if (!file.open(QIODevice::WriteOnly)) { return false; }
#ifdef Q_OS_UNIX
		if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) { return false; }
#endif
		if (file.write(*bytes) != bytes->size() || !current() || !file.commit()) { return false; }
		auto readback = QByteArray();
		if (ReadSyncPrivateFile(AutoPath(_files), kSyncAutoRecordLimit, readback) != SyncStoreStatus::Ready
			|| readback != *bytes) { return false; }
		_recordBytes = *bytes;
	}
	_record = record;
	return current();
}

bool DesktopSyncAutoStore::HoldCheckpoint(const SyncAutoPlan &plan, SyncAutoReason reason) {
	if (!_opened || !_record.active || _record.ticket != plan.ticket
		|| _record.session != plan.record.session
		|| (reason != SyncAutoReason::StateChanged && reason != SyncAutoReason::OwnershipChanged)
		|| _record.event == std::numeric_limits<uint64_t>::max()) { return false; }
	auto record = _record;
	++record.event;
	record.active = false;
	record.held = reason;
	if (std::find(record.noticed.begin(), record.noticed.end(), reason) == record.noticed.end()) {
		record.noticed.push_back(reason);
	}
	return WriteRecord(record, false);
}

SyncAutoPlan DesktopSyncAutoStore::Transition(
		SyncAutoFacts facts, SyncAutoEvent event, int64_t now,
		uint64_t completedTicket, SyncAutoOutcome outcome,
		std::optional<SyncAutoInventory> inventory) {
	if (!Current() || facts.account != _account
		|| (facts.state && StateBytes(*facts.state) != StateBytes(*_local.state()))
		|| _record.event == std::numeric_limits<uint64_t>::max()) { return Refused(_record); }
	const auto local = ReadSyncSettingsFile(_files);
	if (!SameSyncSettingsFile(facts.local, local)) { return Refused(_record); }
	facts.local = local;
	if (facts.local.status == SyncSettingsFileStatus::Present && !facts.local.usingLastGood
		&& !CanApplyAccountSettingsDocument(u"settings.toml"_q, facts.local.text)) {
		facts.local = MakeSyncSettingsFile(SyncSettingsFileStatus::Invalid);
	}
	facts.state = *_local.state();
	if (facts.state->version != 1) { facts.chatResolved = false; }
	facts.accountLock = true;
	_saveSettled = facts.saveDebounceFinished;
	if (_saveSettled && facts.savedFingerprint.isEmpty()
		&& _record.preserved.value(u"desktop_saved_fingerprint"_q).toString() == facts.local.fingerprint
		&& _record.preserved.value(u"desktop_saved_identity"_q).toString().toLatin1() == SyncPathIdentity(_files.settings)) {
		facts.savedFingerprint = facts.local.fingerprint;
	}
	auto plan = PlanSyncAuto({ _record, facts, event, _record.event + 1,
		now, completedTicket, outcome, std::move(inventory) });
	if (_saveSettled && facts.local.status == SyncSettingsFileStatus::Present
		&& !facts.savedFingerprint.isEmpty() && facts.savedFingerprint == facts.local.fingerprint) {
		const auto identity = SyncPathIdentity(_files.settings);
		if (!identity.isEmpty() && identity != "missing") {
			plan.record.preserved.insert(u"desktop_saved_fingerprint"_q, facts.savedFingerprint);
			plan.record.preserved.insert(u"desktop_saved_identity"_q, QString::fromLatin1(identity));
		}
	}
	return WriteRecord(plan.record, true) ? plan : Refused(_record);
}

bool DesktopSyncAutoStore::Authorized(const SyncAutoPlan &plan) const {
	return Current() && _record.active && _record.ticket == plan.ticket
		&& _record.session == plan.record.session && _record.pending == plan.action
		&& _record.commitOnly == plan.commitOnly
		&& _record.expectedState == plan.record.expectedState
		&& _record.expectedFile == plan.record.expectedFile
		&& _record.record == plan.record.record
		&& _record.messageId == plan.messageId;
}

bool DesktopSyncAutoStore::ExecuteLocal(
		const SyncAutoPlan &plan,
		const std::function<bool(SyncLocalStore &)> &apply) {
	if (!Authorized(plan) || !plan.nextState) { return false; }
	const auto expected = StateBytes(*plan.nextState);
	if (expected != plan.record.expectedState) { return false; }
	if (plan.action == SyncAutoAction::Publish) {
		const auto local = ReadSyncSettingsFile(_files);
		const auto payload = InspectConfigPayload(ParseSyncEnvelope(plan.bytes));
		if (!_saveSettled || local.status != SyncSettingsFileStatus::Present || local.usingLastGood
			|| payload.status != ConfigPayloadStatus::Valid || local.text != payload.text
			|| _record.preserved.value(u"desktop_saved_fingerprint"_q).toString() != local.fingerprint
			|| _record.preserved.value(u"desktop_saved_identity"_q).toString().toLatin1() != SyncPathIdentity(_files.settings)) { return false; }
		if (!_local.StageConfig(plan.bytes, plan.nextState->configData, _token)) { return false; }
	} else if (plan.action == SyncAutoAction::AutoApply) {
		if (!apply || !apply(_local)) { return false; }
	} else if (plan.action == SyncAutoAction::Reconcile && plan.commitOnly) {
		if (StateBytes(*_local.state()) == expected) { return true; }
		if (_local.state()->config.pendingSeq) {
			const auto pending = _local.ReadPendingConfig();
			if (!pending || pending.staged != plan.record.record) { return false; }
			const auto parsed = ParseSyncEnvelope(pending.staged);
			const auto payload = InspectConfigPayload(parsed);
			const auto observation = OwnRecordObservation{ OwnRecordObservationKind::Present,
				_local.state()->config.pendingSeq, _local.state()->config.ownHash };
			const auto confirmed = ConfirmConfigReadBack(*_local.state(), _local.state()->createdDevice,
				observation, payload.version);
			auto id = int32_t(0);
			for (const auto &entry : plan.nextState->ownMessages) {
				const auto recorded = RecordConfirmedOwnConfigMessage(confirmed.state, entry.messageId, pending.staged);
				if (recorded && StateBytes(recorded.state) == expected) { id = entry.messageId; break; }
			}
			if (id <= 0 || !_local.ConfirmConfigReadBack(pending.staged, _local.state()->createdDevice, id)) { return false; }
		} else if (plan.record.retiring) {
			const auto next = RemoveAbsentOwnConfigMessage(*_local.state(), plan.messageId, SyncOwnMessagePresence::Absent);
			if (!next || StateBytes(next.state) != expected
				|| !_local.RemoveAbsentConfigMessage(plan.messageId, plan.record.record, _token)) { return false; }
		} else { return false; }
	} else { return false; }
	return _local.state() && StateBytes(*_local.state()) == expected && Current();
}

bool DesktopSyncAutoStore::AuthorizeEffect(const SyncAutoPlan &plan, const SyncAutoServerRead *read) {
	if (!Authorized(plan)) { return false; }
	if (plan.action == SyncAutoAction::Retire) {
		return read && read->messageId == plan.messageId && read->fresh && read->owned
			&& read->presence == SyncAutoMessagePresence::Present && read->record == plan.bytes
			&& CheckOwnConfigMessageDeletion(*_local.state(), plan.messageId, read->record);
	}
	const auto pending = _local.ReadPendingConfig();
	if (!pending || pending.staged != plan.bytes) { return false; }
	if (plan.action == SyncAutoAction::Post) { return !read; }
	return plan.action == SyncAutoAction::EditInPlace && read && read->messageId == plan.messageId
		&& DesktopSyncAutoEditAuthorized(*_local.state(), *read);
}

bool DesktopSyncAutoStore::CommitRetirement(
		const SyncAutoPlan &plan, const SyncAutoServerRead &absence) {
	if (!Authorized(plan) || plan.action != SyncAutoAction::Retire
		|| !absence.fresh || absence.messageId != plan.messageId
		|| absence.presence != SyncAutoMessagePresence::Absent || !absence.record.isEmpty()) { return false; }
	const auto next = RemoveAbsentOwnConfigMessage(*_local.state(), plan.messageId, SyncOwnMessagePresence::Absent);
	return next && StateBytes(next.state) == plan.record.expectedState
		&& _local.RemoveAbsentConfigMessage(plan.messageId, plan.bytes, _token)
		&& Current();
}

const SyncAutoRecord &DesktopSyncAutoStore::record() const { return _record; }
SyncLocalStore &DesktopSyncAutoStore::local() { return _local; }
const AccountFiles &DesktopSyncAutoStore::files() const { return _files; }

}

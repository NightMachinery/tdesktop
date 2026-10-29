#include "purple/purple_sync_chat_setup_flow.h"

namespace Purple {

DesktopSyncChatSetupFlow::DesktopSyncChatSetupFlow(
		DesktopSyncChatSetupSnapshot opening,
		DesktopSyncChatSetupSelection selection, QString directory,
		QString statePath, std::shared_ptr<std::atomic<bool>> admitted,
		Fn<void(DesktopSyncChatSetupProgress)> progress, bool resumeOnly)
: _resumeOnly(resumeOnly)
, _opening(std::move(opening))
, _selection(std::move(selection))
, _directory(std::move(directory))
, _statePath(std::move(statePath))
, _admitted(std::move(admitted))
, _progress(std::move(progress)) {
}

void DesktopSyncChatSetupFlow::open(int64_t now) {
	if (_store || !_admitted || !_admitted->load()) {
		publish(u"setup-context-held"_q);
		return;
	}
	_store = std::make_unique<DesktopSyncChatCreateStore>(
		_directory, DesktopSyncChatSetupAuthority(_opening.scope),
		_opening.scope.origin.account, [current = _admitted] {
			return current->load();
		});
	if (!_store->Open()) {
		publish(_store->error());
		return;
	}
	const auto existing = _store->journal();
	if (!existing && _resumeOnly) {
		_store.reset();
		publish(u"setup-no-journal"_q, true);
		return;
	}
	if (existing && ((!existing->provisionSource.isEmpty() != _selection.provisionOnly)
		|| (_selection.provisionOnly && existing->provisionSource
			!= DesktopSyncChatMoveFingerprint(_opening)))) {
		_store.reset();
		publish(u"setup-purpose-or-source-held"_q);
		return;
	}
	const auto journal = existing ? existing : _selection.provisionOnly
		? BeginDesktopSyncChatProvision(_opening, _selection)
		: BeginDesktopSyncChatCreate(_opening, _selection);
	if (!journal || (!existing && !_store->Save(*journal))) {
		publish(u"setup-intent-held"_q);
		return;
	}
	_executor = std::make_unique<DesktopSyncChatSetupExecutor>(
		_opening.scope.origin, *journal,
		[this](const DesktopSyncChatCreateJournal &next) {
			return _store && _store->Save(next);
		}, [current = _admitted] { return current->load(); });
	if (existing) {
		publish(u"setup-resume-required"_q);
	} else {
		resume(now);
	}
}

void DesktopSyncChatSetupFlow::resume(int64_t now) {
	if (_admitted->load() && (!_executor || (_store && !_store->error().isEmpty()))) {
		_executor.reset();
		_store.reset();
		open(now);
		return;
	}
	if (!_executor || !_admitted->load()) {
		publish(u"setup-context-held"_q);
		return;
	}
	_executor->Start(now);
	advance(now);
}

void DesktopSyncChatSetupFlow::useSelf(int64_t now) {
	if (_executor && _admitted->load()) {
		_executor->UseSelf(now);
		advance(now);
	}
}

void DesktopSyncChatSetupFlow::reconsent(
		const DesktopSyncChatConsentOffer &offer, int64_t now) {
	if (_executor && _admitted->load()) {
		if (_executor->Reconsent(offer, now)) { advance(now); }
	}
}

void DesktopSyncChatSetupFlow::complete(
		const DesktopSyncChatCreateReply &reply, int64_t now) {
	if (_executor && _admitted->load() && _executor->Complete(reply, now)) {
		advance(now);
	}
}

bool DesktopSyncChatSetupFlow::renew(const DesktopSyncChatCreateCommand &command,
		const DesktopSyncChatSetupResumeProof &proof, int64_t now) {
	const auto allowed = _executor && _store && _admitted->load()
		&& _executor->Renew(command, proof, now,
			[this](const DesktopSyncChatCreateJournal &journal) { return commit(journal); });
	if (!allowed && _executor
		&& _executor->phase() != DesktopSyncChatCreatePhase::Running) {
		publish();
	}
	return allowed;
}

void DesktopSyncChatSetupFlow::cancel() {
	if (_admitted) {
		_admitted->store(false);
	}
	if (_executor) {
		_executor->Cancel();
	}
	_store.reset();
	publish();
}

bool DesktopSyncChatSetupFlow::commit(
		const DesktopSyncChatCreateJournal &journal) {
	const auto &scope = _opening.scope;
	const auto disk = OpenDesktopSyncChatDisk(scope.root, _directory,
		_statePath, scope.origin.account, scope.binding, scope.loginBinding,
		_admitted, true);
	const auto record = DesktopSyncChatSetupCache(journal, scope, QString());
	return record && disk->error.isEmpty() && !disk->settingsBound
		&& disk->settingsSpace.isEmpty()
		&& (!disk->cache || disk->cache->mark == record->mark)
		&& SaveDesktopSyncChatDisk(disk, record, _admitted);
}

void DesktopSyncChatSetupFlow::advance(int64_t now) {
	if (!_executor || !_admitted->load()) {
		publish(u"setup-context-held"_q);
		return;
	}
	const auto command = _executor->Next();
	if (command && command->step == DesktopSyncChatCreateStep::Commit) {
		auto reply = DesktopSyncChatCreateReply{
			command->origin, command->operation, command->ticket };
		reply.outcome = commit(command->journal)
			? DesktopSyncChatCreateOutcome::Okay
			: DesktopSyncChatCreateOutcome::Rejected;
		if (_executor->Complete(reply, now)) {
			advance(now);
		}
		return;
	}
	auto status = DesktopSyncChatSetupProgress{
		_executor->phase(), _executor->reason(), _executor->journal(),
		_executor->offeredReaders(), command };
	if (status.phase == DesktopSyncChatCreatePhase::Complete) {
		status.provisioned = !_executor->journal().provisionSource.isEmpty();
		_store.reset();
	}
	if (_progress) {
		_progress(std::move(status));
	}
}

void DesktopSyncChatSetupFlow::publish(QString error, bool resumeAbsent) {
	auto status = DesktopSyncChatSetupProgress();
	status.resumeAbsent = resumeAbsent;
	if (_executor) {
		status.phase = _executor->phase();
		status.reason = _executor->reason();
		status.journal = _executor->journal();
		status.consent = _executor->offeredReaders();
	}
	if (!error.isEmpty()) {
		status.phase = DesktopSyncChatCreatePhase::Held;
		status.reason = std::move(error);
	}
	if (_progress) {
		_progress(std::move(status));
	}
}

}

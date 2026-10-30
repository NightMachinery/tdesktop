#include "purple/purple_sync_chat_carry_executor.h"

#include "base/random.h"
#include "purple/purple_sync_chat_create.h"

#include <algorithm>

namespace Purple {
namespace {

uint64_t RandomId() {
	auto result = uint64_t(0);
	while (!result) { base::RandomFill(&result, sizeof(result)); }
	return result;
}

bool FrozenTargetWinner(const SyncCarryJournal &journal,
		const SyncChatResolution &resolution, const SyncChatCache &winner) {
	return resolution.winner == winner && winner.chat == journal.target
		&& ((resolution.action == SyncChatAction::Use && resolution.readChat == journal.target)
			|| ((resolution.action == SyncChatAction::Follow || resolution.action == SyncChatAction::Fill)
				&& resolution.source == journal.source && resolution.cache == journal.oldMark));
}

}

DesktopSyncChatCarry::DesktopSyncChatCarry(QString directory, SyncChatCarryStoreScope scope,
		DesktopSyncChatOrigin origin, std::shared_ptr<std::atomic<bool>> admitted,
		std::shared_ptr<DesktopSyncCarryGate::Lease> lease,
		Fn<void(DesktopSyncCarryProgress)> progress, SyncDurableFault fault)
: _directory(std::move(directory)), _scope(std::move(scope)), _origin(std::move(origin))
, _admitted(std::move(admitted)), _lease(std::move(lease))
, _progress(std::move(progress)), _fault(std::move(fault)) {
	_run = RandomId();
}

DesktopSyncChatCarry::~DesktopSyncChatCarry() { retire(); }

bool DesktopSyncChatCarry::current() const {
	return SyncStoreWorkerThread() && !_closed && _admitted && _admitted->load()
		&& _lease && _lease->quiescent() && _scope.account == _origin.account;
}

bool DesktopSyncChatCarry::accept(const DesktopSyncCarryCommand &command) const {
	return current() && _store && _store->journal() && _command
		&& command.ticket == _command->ticket && command.run == _run
		&& command.operation == _store->journal()->operationId
		&& command.revision == _store->journal()->revision
		&& command.origin == _origin && command.action == _command->action
		&& command.source == _store->journal()->source && command.target == _store->journal()->target
		&& command.dispatch == _command->dispatch;
}

bool DesktopSyncChatCarry::fresh(const DesktopSyncCarryFresh &facts, bool sameResolution) {
	if (!current() || !_store || !_store->journal() || !facts.current || !facts.current()) { return false; }
	const auto &journal = *_store->journal();
	const auto eligible = DesktopSyncChatEligibility(facts.target.peer);
	const auto resolution = ResolveSyncChat(facts.resolutionInput);
	return current() && ValidDesktopSyncChatIo(facts.target)
		&& facts.source.peer.origin == _origin && facts.target.peer.origin == _origin
		&& facts.source.peer.chat == journal.source && facts.target.peer.chat == journal.target
		&& facts.loginAuthority == _scope.loginAuthority
		&& facts.resolutionInput.selfUserId == _scope.account.userId
		&& (!facts.resolutionInput.cache || (facts.resolutionInput.cacheRead != SyncChatDirectRead::Unknown
			&& facts.resolutionInput.cacheRead != SyncChatDirectRead::Migrated))
		&& (sameResolution
			? resolution.action == journal.expectedAction && resolution.winner == journal.expectedWinner && resolution.cache == journal.oldMark
			: ((journal.expectedAction == SyncChatAction::Searching || journal.expectedAction == SyncChatAction::Lost)
					&& resolution.action == journal.expectedAction && resolution.winner == journal.expectedWinner && resolution.cache == journal.oldMark)
				|| resolution.action == SyncChatAction::Follow || resolution.action == SyncChatAction::Fill
				|| resolution.action == SyncChatAction::NewerMark || (resolution.action == SyncChatAction::Use && !resolution.postingHeld))
		&& eligible
		&& CheckSyncCarryReaders(journal, *eligible) != SyncCarryReaderVerdict::Hold
		&& _io && _io->CheckReaders(facts);
}

void DesktopSyncChatCarry::retire() {
	++_run;
	if (_command) { for (const auto &permit : _command->effects) { permit.revoke(); } }
	_command.reset(); _cleanup.reset(); _io.reset(); _store.reset(); _lease.reset(); _pendingOpening.reset();
	_closed = true;
}

void DesktopSyncChatCarry::publish(QString reason, SyncChatCarryStorageResult value) {
	auto progress = DesktopSyncCarryProgress();
	progress.reason = std::move(reason); progress.storeStatus = value.status; progress.journalStatus = value.journalStatus;
	if (_store) {
		if (_store->foreignLogin() && _scope.binding.isEmpty() && _store->state()) { progress.bindingRecoveryState = *_store->state(); }
		if (_store->journal()) {
			progress.journal = *_store->journal();
			_rememberedJournal = progress.journal;
			progress.terminal = value.status == SyncStoreStatus::Ready && _store->Current()
				&& !SyncCarryJournalHoldsWrites(*progress.journal);
		}
		const auto retained = _store->RetainedEffects();
		if (retained) { progress.retainedEffects = *retained; }
		const auto images = _store->RetainedImages();
		if (images) { progress.retainedImages = *images; }
	}
	if (!progress.journal) { progress.journal = _rememberedJournal; }
	progress.command = _command;
	if (progress.journal) { progress.cleanup = DesktopSyncChatCleanupProgressOf(*progress.journal,_command.has_value(),_cleanupConfirmed,_cleanupUncopied); }
	progress.absent = value.status == SyncStoreStatus::Ready && _store && _store->Current() && !_store->journal();
	if (progress.terminal || progress.absent) { retire(); }
	if (_progress) { _progress(std::move(progress)); }
}

bool DesktopSyncChatCarry::result(SyncChatCarryStorageResult value) {
	if (!value || (value.journalStatus != SyncCarryJournalStatus::Valid && value.journalStatus != SyncCarryJournalStatus::Invalid)) {
		_command.reset();
		publish(value.status == SyncStoreStatus::Ready && value.journalStatus == SyncCarryJournalStatus::Retry
			? u"carry-retry-required"_q : value.status == SyncStoreStatus::MissingLocalVersion
			? u"missing-local-version"_q : u"carry-held"_q, value);
		if (_store && !_store->Current()) { _cleanup.reset(); _io.reset(); _store.reset(); }
		return false;
	}
	return true;
}

SyncCarryJournalEvent DesktopSyncChatCarry::event(SyncCarryEventKind kind) const {
	auto result = SyncCarryEvent(*_store->journal(), kind);
	result.outcome = SyncCarryOutcome::Success;
	return result;
}

void DesktopSyncChatCarry::open(std::optional<SyncCarryJournalStart> start,
		std::optional<DesktopSyncChatPeer> consentPeer, QString stream, std::optional<DesktopSyncCarryOpening> opening) {
	if (!SyncStoreWorkerThread()) { retire(); publish(u"worker-required"_q, {SyncStoreStatus::WrongThread}); return; }
	if (!current() || _store || _pendingOpening) {
		if (!_store && !_pendingOpening && !_closed && _admitted && _admitted->load()) {
			_pendingOpening = PendingOpening{std::move(start), std::move(consentPeer), std::move(stream), std::move(opening)};
		}
		publish(u"publishers-not-quiescent"_q, {SyncStoreStatus::LockBusy}); return;
	}
	_store = std::make_unique<SyncChatCarryStore>(_directory, _scope, _fault);
	if (!result(_store->Open())) { if (!_rememberedJournal || _rememberedJournal->phase!=SyncCarryPhase::Cleanup) { retire(); } return; }
	if (start && opening && (_store->_stateFile.bytes != opening->state || _store->_cacheFile.bytes != opening->cache)) {
		publish(u"opening-images-held"_q); retire(); return;
	}
	if (start) {
		if (start->account != _origin.account || start->preferenceToken != _scope.binding
			|| ((start->mode == SyncCarryMode::Switch || start->mode == SyncCarryMode::RepairMark) && start->markText != DesktopSyncChatMarkText())
			|| (start->loginAuthority != DesktopSyncCarryLogin(_scope.loginAuthority))) {
			publish(u"opening-scope-held"_q); retire(); return;
		}
		if (!result(_store->Begin(*start, consentPeer.has_value(), opening && opening->cleanupSettings))) { retire(); return; }
	} else if (!_store->journal()) { publish(u"no-carry-journal"_q); return; }
	_io = std::make_unique<DesktopSyncChatCarryIo>(*_store);
	if (consentPeer && (!start || consentPeer->origin != _origin || !_io->AcceptReaders(*consentPeer, stream))) {
		publish(u"reader-baseline-held"_q); retire(); return;
	}
	if (_store->foreignLogin() && SyncCarryJournalHoldsWrites(*_store->journal())) {
		publish(u"interrupted-login-discard-only"_q); return;
	}
	if (!start) {
		const auto resumed = _store->Resume();
		_resumeRefused = resumed.journalStatus == SyncCarryJournalStatus::NeedsReview;
		if (!result(resumed)) { return; }
	}
	if (_skipCleanupRequested) { _skipCleanupRequested=false; skipCleanup(true); return; }
	_needsFresh = !start;
	advance();
}

void DesktopSyncChatCarry::resume() {
	if (!current() || _command) { return; }
	if (!_store) {
		auto pending = std::move(_pendingOpening); _pendingOpening.reset();
		if (pending) { open(std::move(pending->start), std::move(pending->consentPeer), std::move(pending->stream), std::move(pending->opening)); }
		else { open(); }
		return;
	}
	if (_store->foreignLogin()) { publish(u"interrupted-login-discard-only"_q); return; }
	if (_resumeRefused) { publish(u"recovery-review-required"_q); return; }
	_needsFresh = true;
	advance();
}

void DesktopSyncChatCarry::issue(SyncCarryJournalAction action, bool dispatch, std::vector<DesktopSyncCarryPermit> effects,
		std::optional<DesktopSyncCleanupRead> cleanupRead, std::optional<DesktopSyncCleanupPermit> cleanupPermit) {
	const auto &journal = *_store->journal();
	auto command = DesktopSyncCarryCommand();
	command.ticket = ++_ticket; command.run = _run; command.operation = journal.operationId;
	command.revision = journal.revision; command.action = action; command.origin = _origin;
	command.source = journal.source; command.target = journal.target;
	command.dispatch = dispatch; command.effects = std::move(effects);
	command.cleanupRead=std::move(cleanupRead); command.cleanupPermit=std::move(cleanupPermit);
	command.markId = _recoveryWinner ? _recoveryWinner->markId
		: journal.targetMark ? journal.targetMark->markId : _io->MarkReceiptId();
	if (action == SyncCarryJournalAction::ResolvePending) {
		const auto pending = _store->Pending();
		if (!pending) { publish(u"pending-unreadable"_q, {pending.status}); return; }
		command.pendingRecord = pending.staged;
	}
	_command = std::move(command);
	publish();
}

void DesktopSyncChatCarry::advance() {
	if (!current() || !_store || !_store->journal()) { return; }
	if (!SyncCarryJournalHoldsWrites(*_store->journal())) { publish(u"carry-terminal"_q); return; }
	if (_needsFresh) { issue(SyncCarryJournalAction::ResolveAgain); return; }
	const auto action = NextSyncCarryJournalAction(*_store->journal());
	if (_recoveryOnly && (action == SyncCarryJournalAction::Deliver
		|| action == SyncCarryJournalAction::PostMark || action == SyncCarryJournalAction::ResolvePending)) {
		publish(u"recovery-reconciliation-only"_q); return;
	}
	switch (action) {
	case SyncCarryJournalAction::WritePendingState:
	case SyncCarryJournalAction::CommitState:
		if (result(_store->CommitPreparedState())) { advance(); }
		return;
	case SyncCarryJournalAction::RetirePendingStage:
	case SyncCarryJournalAction::RetireOwnStage:
		if (result(_store->RetireStage())) { advance(); }
		return;
	case SyncCarryJournalAction::StageOwn:
		if (result(_store->StageOwn())) { advance(); }
		return;
	case SyncCarryJournalAction::PrepareCache:
	case SyncCarryJournalAction::CompleteCache:
		if (!_peer) { issue(action); return; }
		if (result(_store->WriteCache(*_peer, action == SyncCarryJournalAction::CompleteCache))) { advance(); }
		return;
	case SyncCarryJournalAction::None:
		publish(u"invalid-journal-action"_q); return;
	default:
		issue(action); return;
	}
}

void DesktopSyncChatCarry::plan(const DesktopSyncCarryReply &reply) {
	if (!reply.fresh || !fresh(*reply.fresh) || !reply.destination) { publish(u"fresh-plan-held"_q); return; }
	const auto destination = DesktopSyncChatCarryInventory(*reply.destination);
	const auto source = reply.source ? DesktopSyncChatCarryInventory(*reply.source) : std::optional<SyncCarryInventory>();
	const auto &journal = *_store->journal();
	if (!destination || reply.destination->route.peer.origin != _origin
		|| destination->chat != journal.target || (reply.access == SyncCarrySourceAccess::Readable
			&& (!source || source->chat != journal.source || reply.source->route.peer.origin != _origin))) { publish(u"complete-inventory-required"_q); return; }
	auto input = SyncCarryInput();
	input.account = journal.account; input.source = journal.source; input.target = journal.target;
	input.mode = journal.mode; input.access = reply.access; input.preferenceToken = _scope.binding;
	input.targetEligibility = *DesktopSyncChatEligibility(reply.fresh->target.peer);
	input.readerConsentAccepted = true; input.winner = journal.targetMark;
	input.state = _store->state() ? std::optional(*_store->state()) : std::nullopt;
	if (input.state) {
		const auto &library = input.state->library;
		input.playlistsInUse = library.seq || library.confirmedSeq || library.abandonedSeq
			|| !library.ownHash.isEmpty() || !library.issuedRecords.empty()
			|| std::any_of(input.state->ownMessages.begin(), input.state->ownMessages.end(), [](const auto &message) { return message.stream == SyncLocalStream::Library; });
	}
	const auto authority=DesktopSyncCarryTransportOf(_store->_envelope,journal);
	if (!authority) { publish(u"cleanup-authorization-held"_q); return; }
	if (journal.mode==SyncCarryMode::Switch && authority->value(u"cleanup_settings_authorized"_q)==QJsonValue(true)) {
		input.cleanup=SyncCarryCleanup::AllSettings; input.cleanupAuthorized=true;
	} else if (journal.mode==SyncCarryMode::Follow && journal.source!=PeerToken{}) {
		input.cleanup=SyncCarryCleanup::OwnLatePosts; input.cleanupAuthorized=true;
	}
	input.oldInventory = source.value_or(SyncCarryInventory{journal.account, journal.source, false, {}});
	input.newInventory = *destination;
	if (input.state && !input.state->configData.base.isEmpty()) {
		const auto base = _store->OwnBase();
		if (!base) { publish(u"missing-local-version"_q, {base.status}); return; }
		const auto envelope = ParseSyncEnvelope(base.staged);
		const auto payload = InspectConfigPayload(envelope);
		if (!envelope.header || payload.status != ConfigPayloadStatus::Valid) { publish(u"invalid-local-version"_q); return; }
		input.ownVersion = SyncCarryOwnVersion{payload.text, payload.version, envelope.header->writerPlatform,
			envelope.header->writerApp, envelope.header->at};
	}
	const auto prepared = PlanCarry(input);
	if (!prepared) { publish(prepared.status == SyncCarryStatus::PlaylistRequired ? u"playlist-required"_q
		: prepared.status == SyncCarryStatus::MissingLocalVersion ? u"missing-local-version"_q : u"plan-held"_q); return; }
	auto accepted = event(SyncCarryEventKind::PlanPrepared); accepted.plan = prepared.plan;
	if (result(_store->Advance(accepted))) { advance(); }
}

void DesktopSyncChatCarry::prepareEffects(const DesktopSyncCarryReply &reply) {
	if (!reply.fresh || !fresh(*reply.fresh)) { publish(u"before-post-held"_q); return; }
	const auto action = reply.command.action;
	auto identities = std::vector<std::pair<DesktopSyncCarryIdentity, DesktopSyncCarryFresh>>();
	const auto &journal = *_store->journal();
	if (action == SyncCarryJournalAction::Deliver) {
		for (const auto &reference : SyncCarryJournalForwards(journal)) {
			auto facts = *reply.fresh;
			if (!reply.source) { publish(u"source-readback-required"_q); return; }
			const auto found = std::find_if(reply.source->receipts.begin(), reply.source->receipts.end(), [&](const auto &receipt) { return receipt.record.id == reference.messageId; });
			if (found == reply.source->receipts.end()) { publish(u"source-readback-required"_q); return; }
			facts.sourceRecord = *found;
			const auto id = _io->Reserve(DesktopSyncCarryEffect::Forward, reference.recordHash, facts, RandomId());
			if (!id) { publish(u"forward-identity-held"_q); return; }
			identities.emplace_back(*id, facts);
		}
		if (SyncCarryJournalPostsOwnRecord(journal)) {
			const auto id = _io->Reserve(DesktopSyncCarryEffect::OwnRecord, journal.plan->ownRecordHash, *reply.fresh, 0, RandomId());
			if (!id) { publish(u"own-identity-held"_q); return; }
			identities.emplace_back(*id, *reply.fresh);
		}
	} else {
		const auto id = _io->Reserve(DesktopSyncCarryEffect::Mark, {}, *reply.fresh, 0);
		if (!id) { publish(u"mark-identity-held"_q); return; }
		identities.emplace_back(*id, *reply.fresh);
	}
	if (!result(_store->Advance(event(action == SyncCarryJournalAction::Deliver
		? SyncCarryEventKind::DeliveryPrepared : SyncCarryEventKind::MarkPostPrepared)))) { return; }
	auto permits = std::vector<DesktopSyncCarryPermit>();
	for (const auto &[identity, facts] : identities) {
		const auto permit = _io->Authorize(identity.slot, facts);
		if (!permit) { for (const auto &saved : permits) { saved.revoke(); } publish(u"effect-authority-held"_q); return; }
		permits.push_back(*permit);
	}
	if (permits.empty()) { advance(); return; }
	issue(action, true, std::move(permits));
}

void DesktopSyncChatCarry::complete(DesktopSyncCarryReply reply) {
	if (!accept(reply.command)) { return; }
	const auto opening = *_command;
	_command.reset();
	if (reply.fresh) { reply.fresh->current = [admitted = _admitted] { return admitted->load(); }; }
	if (_needsFresh) {
		if (!reply.fresh || reply.outcome != SyncCarryOutcome::Success) {
			publish(u"startup-resolution-held"_q); return;
		}
		if (fresh(*reply.fresh)) {
			_needsFresh = false; _peer = reply.fresh->target.peer; advance(); return;
		}
		const auto &journal = *_store->journal();
		const auto resolution = ResolveSyncChat(reply.fresh->resolutionInput);
		const auto winner = resolution.winner;
		const auto own = journal.mode == SyncCarryMode::Switch || journal.mode == SyncCarryMode::RepairMark;
		const auto id = own ? _io->MarkReceiptId() : journal.targetMark ? journal.targetMark->markId : 0;
		const auto next = NextSyncCarryJournalAction(journal);
		const auto localCompletion = next == SyncCarryJournalAction::VerifyDestination
			|| next == SyncCarryJournalAction::ReadMark
			|| next == SyncCarryJournalAction::PrepareCache || next == SyncCarryJournalAction::CommitState
			|| next == SyncCarryJournalAction::RetireOwnStage || next == SyncCarryJournalAction::CompleteCache
			|| next == SyncCarryJournalAction::Cleanup;
		if (!fresh(*reply.fresh, false) || !localCompletion || !winner
			|| !FrozenTargetWinner(journal, resolution, *winner)
			|| (own ? id > 0 && winner->markId != id : id <= 0 || winner->markId != id)
			|| (own ? !journal.markAttempted : journal.targetMark != winner)
			|| (journal.markVerified && journal.targetMark != winner)) {
			publish(u"startup-resolution-held"_q); return;
		}
		_needsFresh = false;
		_recoveryOnly = true;
		_recoveryWinner = winner;
		issue(SyncCarryJournalAction::ReadMark);
		return;
	}
	if (opening.action==SyncCarryJournalAction::Cleanup && opening.dispatch) {
		if (opening.cleanupPermit) { opening.cleanupPermit->revoke(); }
		if (reply.outcome!=SyncCarryOutcome::Success || !reply.cleanupResult) { cleanupHold(u"cleanup-delete-uncertain"_q); return; }
		issue(SyncCarryJournalAction::Cleanup); return;
	}
	if (opening.dispatch) {
		auto outcome = reply.outcome;
		if (outcome == SyncCarryOutcome::Success && reply.updates.size() != opening.effects.size()) { outcome = SyncCarryOutcome::Unknown; }
		for (auto i = size_t(0); i != opening.effects.size(); ++i) {
			const auto &permit = opening.effects[i];
			if (outcome == SyncCarryOutcome::Success && !_io->RecordReceipt(permit, reply.updates[i])) { outcome = SyncCarryOutcome::Unknown; }
			if (outcome != SyncCarryOutcome::Success) { static_cast<void>(_io->OutcomeUnknown(permit)); }
			permit.revoke();
		}
		if (opening.action == SyncCarryJournalAction::ResolvePending) {
			issue(SyncCarryJournalAction::ResolvePending); return;
		}
		auto accepted = event(opening.action == SyncCarryJournalAction::Deliver ? SyncCarryEventKind::DeliveryResult : SyncCarryEventKind::MarkPostResult);
		accepted.outcome = outcome;
		const auto persisted = _store->Advance(accepted);
		if (!persisted) { static_cast<void>(result(persisted)); return; }
		advance(); return;
	}
	if (opening.action == SyncCarryJournalAction::ResolvePending
		&& reply.sendIntent == SyncCarrySendIntent::Issued && !_io->RememberPendingIssued()) {
		publish(u"pending-intent-held"_q); return;
	}
	if (reply.outcome != SyncCarryOutcome::Success) {
		publish(u"transport-transient-or-unknown"_q); return;
	}
	if (reply.fresh) {
		const auto &facts = *reply.fresh;
		if (!facts.current || !facts.current() || facts.source.peer.origin != _origin || facts.target.peer.origin != _origin
			|| facts.source.peer.chat != opening.source || facts.target.peer.chat != opening.target
			|| facts.loginAuthority != _scope.loginAuthority || !_io->CheckReaders(facts)) {
			if (opening.action==SyncCarryJournalAction::Cleanup) { cleanupHold(u"reader-authority-held"_q); }
			else { publish(u"reader-authority-held"_q); }
			return;
		}
		if (fresh(facts)) { _peer = facts.target.peer; }
	}
	switch (opening.action) {
	case SyncCarryJournalAction::ResolvePending: {
		const auto issued = _io->PendingIssued();
		if (!issued) { publish(u"pending-intent-held"_q); return; }
		auto accepted = event(SyncCarryEventKind::PendingPrepared);
		accepted.stagedRecord = opening.pendingRecord;
		accepted.retainedLocalText = reply.retainedLocalText;
		accepted.oldRead = reply.oldRead; accepted.pendingAccess = reply.access; accepted.pendingSendIntent = *issued ? SyncCarrySendIntent::Issued : reply.sendIntent;
		if (reply.source) {
			const auto source = DesktopSyncChatCarryInventory(*reply.source);
			if (!source || reply.source->route.peer.origin != _origin || source->chat != opening.source) { publish(u"pending-read-held"_q); return; }
			accepted.pendingInventory = *source;
			const auto hash = SyncCarryRecordHash(opening.pendingRecord);
			const auto found = std::find_if(source->messages.begin(), source->messages.end(), [&](const auto &message) { return message.reference.recordHash == hash && message.ownership == SyncChatMessageOwnership::Own; });
			if (found != source->messages.end()) { accepted.pendingReadback = *found; }
			else if (reply.access == SyncCarrySourceAccess::Readable) {
				if (!reply.fresh || !fresh(*reply.fresh)) { publish(u"pending-fresh-held"_q); return; }
				const auto permit = _io->PendingPermit(*reply.fresh,*reply.source,RandomId(),RandomId());
				if (!permit) { publish(u"pending-send-held"_q); return; }
				issue(SyncCarryJournalAction::ResolvePending,true,{*permit}); return;
			}
		}
		if (reply.source && !_io->ConsumePendingRead(*reply.source)) { publish(u"pending-read-generation-held"_q); return; }
		if (result(_store->Advance(accepted))) { advance(); }
	} return;
	case SyncCarryJournalAction::ResolveAgain: {
		if (!reply.fresh || !fresh(*reply.fresh, false)) { publish(u"resolution-held"_q); return; }
		auto accepted = event(SyncCarryEventKind::Resolution);
		accepted.resolution = ResolveSyncChat(reply.fresh->resolutionInput);
		if (result(_store->Advance(accepted))) { advance(); }
	} return;
	case SyncCarryJournalAction::Plan: plan(reply); return;
	case SyncCarryJournalAction::Deliver:
	case SyncCarryJournalAction::PostMark: prepareEffects(reply); return;
	case SyncCarryJournalAction::VerifyDestination: {
		if (!reply.destination || reply.destination->route.peer.origin != _origin || reply.destination->route.peer.chat != opening.target) { publish(u"destination-read-held"_q); return; }
		const auto destination = DesktopSyncChatCarryInventory(*reply.destination);
		if (!destination) { publish(u"complete-destination-required"_q); return; }
		if (!reply.fresh || !_io->ReconcileAll(*reply.destination, *reply.fresh)) { publish(u"delivery-reconciliation-held"_q); return; }
		if (_store->journal()->plan && VerifySyncCarryReadback(*_store->journal()->plan, *destination, reply.playlist).status == SyncCarryStatus::PlaylistRequired) { publish(u"playlist-required"_q); return; }
		auto accepted = event(SyncCarryEventKind::DestinationRead); accepted.destination = *destination;
		accepted.playlist = reply.playlist;
		if (result(_store->Advance(accepted))) { advance(); }
	} return;
	case SyncCarryJournalAction::ReadMark: {
		if (!reply.fresh || !reply.markResponse || reply.fresh->target.peer.origin != _origin
			|| reply.fresh->target.peer.chat != opening.target) { publish(u"exact-mark-required"_q); return; }
		const auto journal = *_store->journal();
		if (journal.mode == SyncCarryMode::Switch || journal.mode == SyncCarryMode::RepairMark) {
			if (!reply.updates.empty()) {
				if (reply.updates.size() != 1 || !_io->RetainedMarkReceipt(*reply.fresh, reply.updates.front())) { publish(u"matched-mark-receipt-required"_q); return; }
			}
			if (_io->MarkReceiptId() <= 0 || _io->MarkReceiptId() != reply.markId) { publish(u"matched-mark-receipt-required"_q); return; }
		} else if (!journal.targetMark || reply.markId != journal.targetMark->markId) { publish(u"winner-mark-mismatch"_q); return; }
		const auto read = DesktopSyncChatCarryMarkReadback(reply.fresh->target, reply.markId, *reply.markResponse, _store->journal()->markText);
		if (!read) { publish(u"mark-unknown"_q); return; }
		if (_recoveryWinner) {
			const auto resolution = ResolveSyncChat(reply.fresh->resolutionInput);
			if (!fresh(*reply.fresh, false) || resolution.winner != _recoveryWinner
				|| !FrozenTargetWinner(journal, resolution, *_recoveryWinner) || read->messageId != _recoveryWinner->markId
				|| read->date != _recoveryWinner->markDate || read->chat != _recoveryWinner->chat) {
				publish(u"recovery-own-mark-held"_q); return;
			}
			_recoveryWinner.reset();
			_peer = reply.fresh->target.peer;
			if (NextSyncCarryJournalAction(journal) != SyncCarryJournalAction::ReadMark) {
				const auto priorWinner = journal.mode == SyncCarryMode::Follow || journal.mode == SyncCarryMode::Fill;
				const auto targetMatches = journal.targetMark == resolution.winner;
				const auto verifyAgain = NextSyncCarryJournalAction(journal) == SyncCarryJournalAction::VerifyDestination
					&& (priorWinner ? targetMatches : journal.markAttempted && (!journal.targetMark || targetMatches));
				if (!(journal.markVerified && targetMatches) && !verifyAgain) {
					publish(u"recovery-own-mark-held"_q); return;
				}
				advance(); return;
			}
		}
		auto accepted = event(SyncCarryEventKind::MarkRead); accepted.mark = *read;
		accepted.noRequestInFlight = true;
		accepted.neverLandedConfirmed = reply.neverLandedConfirmed;
		accepted.neverLandedRequestId = reply.neverLandedRequestId;
		_peer = reply.fresh->target.peer;
		if (result(_store->Advance(accepted))) { advance(); }
	} return;
	case SyncCarryJournalAction::PrepareCache:
	case SyncCarryJournalAction::CompleteCache:
		if (!reply.fresh || !_peer) { publish(u"cache-peer-held"_q); return; }
		advance(); return;
	case SyncCarryJournalAction::Cleanup: cleanup(reply); return;
	default: publish(u"unexpected-transport-action"_q); return;
	}
}

void DesktopSyncChatCarry::cleanupHold(QString reason) {
	_command.reset();
	if (_store && (!_store->Current() || _store->_lastFailure != SyncStoreStatus::Ready)) {
		const auto failure = _store->_lastFailure != SyncStoreStatus::Ready
			? _store->_lastFailure : SyncStoreStatus::Changed;
		static_cast<void>(result({failure}));
		return;
	}
	publish(std::move(reason));
}

void DesktopSyncChatCarry::cleanup(const DesktopSyncCarryReply &reply) {
	const auto &opening=reply.command;
	if (reply.access==SyncCarrySourceAccess::ChatGone && reply.oldRead==SyncChatDirectRead::ChatGone) {
		if (!reply.fresh || !fresh(*reply.fresh,false) || !_store->WitnessCleanup()) { cleanupHold(u"cleanup-gone-scope-held"_q); return; }
		auto accepted=event(SyncCarryEventKind::CleanupChatGone); accepted.noRequestInFlight=true;
		accepted.mark={SyncCarryMarkOutcome::ChatGone,_origin.account,opening.source};
		if (result(_store->Advance(accepted))) { advance(); } return;
	}
	if (!reply.destination || !reply.fresh) { cleanupHold(u"cleanup-read-required"_q); return; }
	if (!_cleanup) { _cleanup=std::make_unique<DesktopSyncChatCleanup>(*_store,_origin,_admitted); }
	if (!_cleanup->prepare(*reply.fresh,*reply.destination,reply.playlist)) { cleanupHold(u"cleanup-verification-held"_q); return; }
	if (opening.cleanupRead) {
		if (!reply.cleanupResponse || !_cleanup->inspect(*opening.cleanupRead,*reply.fresh,*reply.destination,reply.playlist,*reply.cleanupResponse,reply.fresh->sourceRecord)) {
			cleanupHold(u"cleanup-exact-held"_q); return;
		}
		if (const auto permit=_cleanup->permit()) {
			issue(SyncCarryJournalAction::Cleanup,true,{}, {},permit); return;
		}
	}
	const auto deleted=_cleanup->deleted(); if (!deleted) { cleanupHold(u"cleanup-ledger-held"_q); return; }
	_cleanupConfirmed=deleted->size(); _cleanupUncopied=_cleanup->uncopied();
	if (const auto read=_cleanup->next()) {
		issue(SyncCarryJournalAction::Cleanup,false,{},read); return;
	}
	if (!_store->Current()) { cleanupHold(u"cleanup-storage-held"_q); return; }
	const auto destination=DesktopSyncChatCarryInventory(*reply.destination); if (!destination) { cleanupHold(u"cleanup-destination-held"_q); return; }
	auto accepted=event(SyncCarryEventKind::CleanupCompleted); accepted.destination=*destination; accepted.playlist=reply.playlist;
	accepted.deleted=*deleted; accepted.mark=_cleanup->oldMarkAbsent();
	if (result(_store->Advance(accepted))) { advance(); }
}

std::optional<DesktopSyncCleanupRead> DesktopSyncChatCarry::beginCleanupRenew(
		const DesktopSyncCarryCommand &command, const DesktopSyncCleanupPermit &permit) {
	if (!accept(command) || command.action!=SyncCarryJournalAction::Cleanup || !command.dispatch
		|| !command.cleanupPermit || !permit.operationCurrent() || !_cleanup || command.cleanupPermit->item()!=permit.item()) { return {}; }
	const auto read=_cleanup->next();
	return read && read->item==permit.item() ? read : std::nullopt;
}
std::optional<DesktopSyncCleanupPermit> DesktopSyncChatCarry::renewCleanup(const DesktopSyncCarryCommand &command,
		const DesktopSyncCleanupPermit &permit, const DesktopSyncCleanupRead &read, const DesktopSyncCarryReply &reply) {
	if (!accept(command) || !command.dispatch || !_cleanup || !command.cleanupPermit || command.cleanupPermit->item()!=permit.item()
		|| !permit.operationCurrent() || read.item!=permit.item() || !reply.fresh || !reply.destination || !reply.cleanupResponse) { return {}; }
	auto facts=*reply.fresh; facts.current=[admitted=_admitted] { return admitted->load(); };
	if (!fresh(facts,false) || !_cleanup->prepare(facts,*reply.destination,reply.playlist)
		|| !_cleanup->inspect(read,facts,*reply.destination,reply.playlist,*reply.cleanupResponse,facts.sourceRecord)) { return {}; }
	const auto next=_cleanup->permit(); return next && permit.successor(*next) ? next : std::nullopt;
}

std::optional<DesktopSyncCarryPermit> DesktopSyncChatCarry::renew(const DesktopSyncCarryCommand &command,
		const DesktopSyncCarryPermit &permit, const DesktopSyncCarryFresh &facts) {
	auto bound = facts;
	bound.current = [admitted = _admitted] { return admitted->load(); };
	return accept(command) && command.dispatch && fresh(bound) ? _io->Renew(permit, bound) : std::nullopt;
}

void DesktopSyncChatCarry::cancel() {
	if (!_closed && SyncStoreWorkerThread()) {
		_closed = true;
		const auto cleanup = gsl::finally([&] { retire(); });
		if (_store && _store->journal()) {
			auto cancelled = event(SyncCarryEventKind::Cancel);
			cancelled.noRequestInFlight = !_command || !_command->dispatch;
			cancelled.noPendingFiles = !_store->state() || !_store->state()->config.pendingSeq;
			static_cast<void>(_store->Advance(cancelled));
		}
		publish(u"carry-cancelled-retained"_q);
		retire();
	}
}

void DesktopSyncChatCarry::discardInterrupted(bool confirmed) {
	if (!current() || !_store || _command) { return; }
	if (result(_store->DiscardInterrupted(confirmed))) { publish(u"interrupted-effects-retained"_q); }
}

void DesktopSyncChatCarry::skipCleanup(bool confirmed) {
	if (!confirmed || !current() || (_command && _command->dispatch)) { return; }
	_command.reset(); _cleanup.reset();
	if (!_store) { _skipCleanupRequested=true; open(); return; }
	if (!_store->journal() || !_store->WitnessCleanup()) { cleanupHold(u"cleanup-skip-held"_q); return; }
	const auto ledger=_store->CleanupLedger(); if (!ledger) { cleanupHold(u"cleanup-ledger-held"_q); return; }
	auto skipped=event(SyncCarryEventKind::SkipCleanup);
	for (const auto &v:*ledger) { const auto e=v.toObject(); if (e.value(u"state"_q)==QJsonValue(u"absent"_q)) {
		skipped.deleted.push_back({_origin.account,_store->journal()->source,e.value(u"id"_q).toInt(),e.value(u"hash"_q).toString()});
	} }
	skipped.skipCleanupConfirmed=true; skipped.noRequestInFlight=true;
	if (result(_store->Advance(skipped))) { publish(u"cleanup-skipped"_q); }
}

}

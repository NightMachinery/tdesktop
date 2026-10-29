#include <array>

#define PURPLE_SYNC_CHAT_CARRY_EXECUTOR_EMBEDDED
#include "test_sync_chat_carry.cpp"

namespace {

void ReviewDiscardWindow() {
	for (auto retired = 0; retired != 2; ++retired) {
		auto f = ExecutorFiles(true, SyncCarryMode::Switch, true);
		f.engine->open(f.start);
		while (f.progress.command && f.progress.command->action != SyncCarryJournalAction::ReadMark) { f.step(); }
		Must(f.progress.command);
		f.armed = true;
		f.failPoint = retired ? SyncDurablePoint::BeforeWrite : SyncDurablePoint::AfterRename;
		f.failWrite = f.writes + (retired ? 6 : 5);
		f.step();
		const auto saved = InspectSyncCarryJournal(CaptureSyncPrivateFile(f.root + u"/carry.json"_q, 4 * 1024 * 1024).bytes, f.scope.binding);
		Must(saved);
		const auto stateBytes = CaptureSyncPrivateFile(f.root + u"/state.json"_q, 4 * 1024 * 1024).bytes;
		const auto state = ParseSyncLocalState(stateBytes); Must(state);
		Must(saved->stateCommitted && !saved->targetPendingRecord.isEmpty());
		const auto target = f.root + u"/pending/config-"_q + QString::number(state.state.config.pendingSeq) + u".json"_q;
		const auto own = ParseSyncEnvelope(saved->plan->ownRecord); Must(own.header);
		const auto old = f.root + u"/pending/config-"_q + QString::number(own.header->seq) + u".json"_q;
		CHECK(CaptureSyncPrivateFile(target, 1024 * 1024).absent());
		CHECK(CaptureSyncPrivateFile(old, 1024 * 1024).absent() == bool(retired));
		f.armed = f.failing = false;
		f.engine.reset(); f.scope.loginAuthority = QString(64, u'b'); f.fresh.loginAuthority = f.scope.loginAuthority;
		f.fresh.source.peer.origin.login = u"relogin"_q; f.fresh.target.peer.origin.login = u"relogin"_q;
		f.newEngine(true);
		f.engine->discardInterrupted(false);
		CHECK(!f.progress.terminal && CaptureSyncPrivateFile(target, 1024 * 1024).absent());
		f.engine->discardInterrupted(true);
		CHECK(f.progress.terminal);
		CHECK(f.progress.journal && f.progress.journal->phase == SyncCarryPhase::InterruptedAbandoned);
		CHECK(CaptureSyncPrivateFile(target, 1024 * 1024).bytes == saved->targetPendingRecord);
		CHECK(CaptureSyncPrivateFile(old, 1024 * 1024).absent());
		CHECK(CaptureSyncPrivateFile(f.root + u"/state.json"_q, 4 * 1024 * 1024).bytes == stateBytes);
		CHECK(!f.gate->postingHeld() && !f.progress.retainedEffects.empty());
		CHECK(!f.progress.retainedImages.empty());
		if (f.progress.terminal) {
			auto local = SyncLocalStore(f.root); Must(local.Open(true, f.scope.binding));
			CHECK(local.ReadPendingConfig().staged == saved->targetPendingRecord);
		}
		f.engine->cancel();
	}
}

void ReviewDiscardRefusalsAndFailures() {
	for (auto control = 0; control != 9; ++control) {
		auto f = ExecutorFiles(true, SyncCarryMode::Switch, true); f.engine->open(f.start);
		while (f.progress.command && f.progress.command->action != SyncCarryJournalAction::ReadMark) { f.step(); }
		Must(f.progress.command);
		f.armed = true; f.failPoint = SyncDurablePoint::AfterRename; f.failWrite = f.writes + 5; f.step();
		const auto saved = InspectSyncCarryJournal(CaptureSyncPrivateFile(f.root + u"/carry.json"_q, 4 * 1024 * 1024).bytes, f.scope.binding); Must(saved);
		const auto stateBytes = CaptureSyncPrivateFile(f.root + u"/state.json"_q, 4 * 1024 * 1024).bytes;
		const auto state = ParseSyncLocalState(stateBytes); Must(state && saved->stateCommitted);
		const auto target = f.root + u"/pending/config-"_q + QString::number(state.state.config.pendingSeq) + u".json"_q;
		const auto own = ParseSyncEnvelope(saved->plan->ownRecord); Must(own.header);
		const auto old = f.root + u"/pending/config-"_q + QString::number(own.header->seq) + u".json"_q;
		f.armed = f.failing = false; f.engine.reset();
		if (control == 0) Must(WriteSyncPrivateFile(old, saved->plan->ownRecord + ' ') == SyncStoreStatus::Ready);
		if (control == 1) Must(WriteSyncPrivateFile(target, saved->targetPendingRecord + ' ') == SyncStoreStatus::Ready);
		if (control == 2) Must(WriteSyncPrivateFile(f.root + u"/pending/unrelated"_q, "sentinel") == SyncStoreStatus::Ready);
		const auto beforeOld = CaptureSyncPrivateFile(old, 1024 * 1024);
		const auto beforeTarget = CaptureSyncPrivateFile(target, 1024 * 1024);
		f.scope.loginAuthority = QString(64, u'b'); f.fresh.loginAuthority = f.scope.loginAuthority;
		f.fresh.source.peer.origin.login = u"relogin"_q; f.fresh.target.peer.origin.login = u"relogin"_q;
		f.newEngine(true);
		if (control >= 3) {
			const auto points = std::array{SyncDurablePoint::BeforeWrite, SyncDurablePoint::PartialWrite,
				SyncDurablePoint::BeforeRename, SyncDurablePoint::AfterRename,
				SyncDurablePoint::FileBarrier, SyncDurablePoint::DirectoryBarrier};
			f.armed = true; f.failWrite = f.writes + 1; f.failPoint = points[control - 3];
		}
		f.engine->discardInterrupted(true);
		CHECK(!f.progress.terminal && !CaptureSyncPrivateFile(f.root + u"/carry.json"_q, 4 * 1024 * 1024).absent());
		CHECK(CaptureSyncPrivateFile(f.root + u"/state.json"_q, 4 * 1024 * 1024).bytes == stateBytes);
		if (control < 3) {
			CHECK(CaptureSyncPrivateFile(old, 1024 * 1024).bytes == beforeOld.bytes);
			CHECK(CaptureSyncPrivateFile(target, 1024 * 1024).bytes == beforeTarget.bytes);
		} else {
			CHECK(f.failing);
			f.armed = f.failing = false; f.newEngine(true); f.engine->discardInterrupted(true);
			CHECK(f.progress.terminal && f.progress.journal->phase == SyncCarryPhase::InterruptedAbandoned);
			CHECK(CaptureSyncPrivateFile(target, 1024 * 1024).bytes == saved->targetPendingRecord);
			CHECK(CaptureSyncPrivateFile(old, 1024 * 1024).absent());
		}
		f.engine->cancel();
	}
}

void SaveOldCache(ExecutorFiles &f) {
	auto cache = DesktopSyncChatCacheRecord(); cache.account = f.scope.account; cache.mark = *f.start.resolution.cache;
	cache.readers = DesktopSyncChatEligibility(f.fresh.source.peer)->readers;
	cache.readerConsent = true; cache.loginBinding = f.scope.loginAuthority;
	const auto bytes = SerializeDesktopSyncChatCache(cache); Must(bytes);
	Must(WriteSyncPrivateFile(f.scope.cacheFile, *bytes) == SyncStoreStatus::Ready);
}

void ReviewPendingFloor() {
	for (auto restart = 0; restart != 2; ++restart) {
		auto f = ExecutorFiles(true, SyncCarryMode::Switch, true);
		f.own = false; f.pendingPresent = false; f.fresh.source.peer.facts->protectedContent = false;
		SaveOldCache(f);
		f.engine->open(f.start); f.step(); Must(f.progress.command && f.progress.command->dispatch);
		auto unknown = DesktopSyncCarryReply(); unknown.command = *f.progress.command; unknown.outcome = SyncCarryOutcome::Unknown;
		f.engine->complete(unknown); Must(f.progress.command);
		if (restart) { f.newEngine(true); f.step(); Must(f.progress.command); }
		const auto before = CaptureSyncPrivateFile(f.root + u"/state.json"_q, 4 * 1024 * 1024).bytes;
		auto changed = f.reply(); changed.access = SyncCarrySourceAccess::Protected;
		changed.sendIntent = SyncCarrySendIntent::NotIssued; changed.source.reset();
		changed.fresh->source.peer.facts->protectedContent = true;
		f.engine->complete(changed);
		CHECK(f.progress.journal && f.progress.journal->phase == SyncCarryPhase::ResolvePending);
		CHECK(f.progress.journal && f.progress.journal->deferredRecord.isEmpty());
		CHECK(!f.progress.command && !f.progress.terminal);
		CHECK(CaptureSyncPrivateFile(f.root + u"/state.json"_q, 4 * 1024 * 1024).bytes == before);
		f.engine->cancel();
	}
	for (auto incomplete = 0; incomplete != 2; ++incomplete) {
		auto f = ExecutorFiles(true, SyncCarryMode::Switch, true);
		f.engine->open(f.start); Must(f.progress.command);
		auto observed = f.reply(); observed.sendIntent = SyncCarrySendIntent::Issued;
		if (incomplete) { observed.source->inventory.scan.status = SyncHistoryScanStatus::InvalidResponse; }
		else { observed.source.reset(); }
		f.engine->complete(observed); CHECK(!f.progress.command && !f.progress.terminal);
		f.newEngine(true); f.step(); Must(f.progress.command);
		auto downgrade = f.reply(); downgrade.sendIntent = SyncCarrySendIntent::NotIssued; downgrade.source.reset();
		f.engine->complete(downgrade);
		CHECK(f.progress.journal && f.progress.journal->phase == SyncCarryPhase::ResolvePending);
		CHECK(!f.progress.command && !f.progress.terminal);
		f.engine->cancel();
	}
}

void ReviewOwnWinner() {
	for (auto mismatch = 0; mismatch != 4; ++mismatch) {
		auto f = ExecutorFiles(); f.engine->open(f.start);
		while (f.progress.command && f.progress.command->action != SyncCarryJournalAction::ReadMark) { f.step(); }
		Must(f.progress.command); Must(f.markWrites == 1);
		f.actualResolution();
		if (mismatch == 1) f.fresh.resolutionInput.marks.push_back({{f.fresh.target.peer.chat,91,600},SyncChatMarkStatus::Supported,
			*DesktopSyncChatEligibility(f.fresh.target.peer),SyncChatRecordPresence::Present,true,true});
		if (mismatch == 2) f.fresh.target.peer.origin.login = u"wrong"_q;
		const auto writes = f.outward;
		f.newEngine(true); Must(f.progress.command); f.step();
		if (mismatch == 0) {
			CHECK(f.progress.command && f.progress.command->action == SyncCarryJournalAction::ReadMark);
			if (f.progress.command) { f.finish(); }
			CHECK(f.progress.terminal && f.outward == writes && f.markWrites == 1);
		} else if (mismatch == 3 && f.progress.command) {
			auto wrong = f.reply(); wrong.markResponse = Response(f.fresh.target, {CarryMark(f.fresh.target, u"changed"_q)});
			f.engine->complete(wrong); CHECK(!f.progress.command && !f.progress.terminal);
		} else { CHECK(!f.progress.command && !f.progress.terminal); }
		CHECK(f.markWrites == 1 && f.outward == writes);
		f.engine->cancel();
	}
}

void ReviewCleanupGone() {
	for (auto control = 0; control != 7; ++control) {
		auto f = ExecutorFiles(); f.start.removeOldMarkAuthorized = true; f.engine->open(f.start);
		while (f.progress.command && f.progress.command->action != SyncCarryJournalAction::Cleanup) { f.step(); }
		Must(f.progress.command);
		auto gone = f.reply(); gone.access = SyncCarrySourceAccess::ChatGone; gone.oldRead = SyncChatDirectRead::ChatGone;
		if (control == 1) gone.access = SyncCarrySourceAccess::Unknown;
		if (control == 2) gone.oldRead = SyncChatDirectRead::MarkGone;
		if (control == 3) gone.access = SyncCarrySourceAccess::Protected;
		if (control == 4) gone.fresh->source.peer.chat = {PeerTokenKind::Chat, 999};
		if (control == 5) ++gone.fresh->source.peer.origin.account.userId;
		if (control == 6) gone.outcome = SyncCarryOutcome::Unknown;
		f.engine->complete(gone);
		CHECK(f.progress.terminal == (control == 0));
		CHECK(f.progress.journal && f.progress.journal->cleanupChatGone == (control == 0));
		if (control) { CHECK(f.progress.journal && f.progress.journal->phase == SyncCarryPhase::Cleanup); }
		f.engine->cancel();
	}
}

void ReviewUploadConstructor() {
	auto f = ExecutorFiles(true); f.engine->open(f.start);
	while (f.progress.command && !(f.progress.command->action == SyncCarryJournalAction::Deliver && f.progress.command->dispatch)) { f.step(); }
	Must(f.progress.command && f.progress.command->effects.size() == 1);
	const auto p = f.progress.command->effects.front(); const auto q = p;
	auto session = UploadSession();
	const auto done = [](std::optional<MsgId>) {};
	const auto current = [] { return true; };
	const auto renewal = Fn<void(Fn<void(std::optional<DesktopSyncCarryPermit>)>)>();
	auto first = std::make_shared<UploadProbe>(session, p, done, current, renewal);
	auto second = std::make_shared<UploadProbe>(session, q, done, current, renewal);
	CHECK(first->Current()); CHECK(!second->Current());
	const auto fresh = f.engine->renew(*f.progress.command, p, f.fresh); CHECK(fresh);
	if (fresh) {
		auto copied = std::make_shared<UploadProbe>(session, *fresh, done, current, renewal);
		CHECK(!copied->Current());
	}
	CHECK(p.randomId() == q.randomId() && p.fileId() == q.fileId());
	auto unknown = DesktopSyncCarryReply(); unknown.command = *f.progress.command;
	unknown.outcome = SyncCarryOutcome::Unknown; f.engine->complete(unknown);
	CHECK(!p.current() && !first->Current());
	Must(f.progress.command && f.progress.command->action == SyncCarryJournalAction::VerifyDestination);
	f.step(); CHECK(f.progress.reason == u"carry-retry-required"_q);
	f.engine->resume(); f.step(); f.step();
	Must(f.progress.command && f.progress.command->dispatch && f.progress.command->effects.size() == 1);
	const auto retry = f.progress.command->effects.front();
	CHECK(retry.randomId() == p.randomId() && retry.fileId() == p.fileId());
	auto permitted = std::make_shared<UploadProbe>(session, retry, done, current, renewal);
	CHECK(permitted->Current());
	auto obsolete = std::make_shared<UploadProbe>(session, p, done, current, renewal);
	CHECK(!obsolete->Current());
	f.engine->cancel(); CHECK(!first->Current() && !second->Current() && !permitted->Current());
}

void PendingUnknownObservation() {
 for (auto reopen : {false, true}) {
  auto f = ExecutorFiles(true, SyncCarryMode::Switch, true); f.engine->open(f.start); Must(f.progress.command);
  auto observed = f.reply(); observed.sendIntent = SyncCarrySendIntent::Issued;
  observed.source.reset(); observed.outcome = SyncCarryOutcome::Unknown;
  f.engine->complete(observed);
  CHECK(!f.progress.command && !f.progress.terminal);
  if (reopen) { f.newEngine(true); } else { f.engine->resume(); }
  f.step(); Must(f.progress.command);
  auto downgrade = f.reply(); downgrade.sendIntent = SyncCarrySendIntent::NotIssued; downgrade.source.reset();
  f.engine->complete(downgrade);
  std::printf("UNKNOWN ISSUED reopen=%d phase=%d deferred=%d reason=%s\n",reopen,
   int(f.progress.journal->phase), !f.progress.journal->deferredRecord.isEmpty(),qPrintable(f.progress.reason));
  CHECK(f.progress.journal && f.progress.journal->phase == SyncCarryPhase::ResolvePending);
  CHECK(f.progress.journal && f.progress.journal->deferredRecord.isEmpty());
  f.engine->cancel();
 }
}

auto PendingLocal(ExecutorFiles &f) {
	const auto state = CaptureSyncPrivateFile(f.root + u"/state.json"_q, 4 * 1024 * 1024);
	const auto parsed = ParseSyncLocalState(state.bytes); Must(parsed);
	return CaptureSyncPrivateFile(f.root + u"/pending/config-"_q
		+ QString::number(parsed.state.config.pendingSeq) + u".json"_q, 1024 * 1024);
}

void PendingUnknownStateControls() {
	for (const auto reopen : {false, true}) {
		auto f = ExecutorFiles(true, SyncCarryMode::Switch, true);
		f.engine->open(f.start); Must(f.progress.command);
		const auto state = CaptureSyncPrivateFile(f.root + u"/state.json"_q, 4 * 1024 * 1024).bytes;
		const auto pending = PendingLocal(f);
		CHECK(pending.status == SyncStoreStatus::Ready);
		auto observed = f.reply(); observed.sendIntent = SyncCarrySendIntent::Issued;
		observed.source.reset(); observed.outcome = SyncCarryOutcome::Unknown;
		f.engine->complete(observed);
		CHECK(!f.progress.command && !f.progress.terminal);
		CHECK(f.progress.reason == u"transport-transient-or-unknown"_q);
		if (reopen) { f.newEngine(true); } else { f.engine->resume(); }
		f.step(); Must(f.progress.command);
		auto downgrade = f.reply(); downgrade.sendIntent = SyncCarrySendIntent::NotIssued;
		downgrade.source.reset(); f.engine->complete(downgrade);
		CHECK(!f.progress.command && !f.progress.terminal);
		CHECK(f.progress.journal && f.progress.journal->phase == SyncCarryPhase::ResolvePending);
		CHECK(f.progress.journal && f.progress.journal->deferredRecord.isEmpty());
		CHECK(CaptureSyncPrivateFile(f.root + u"/state.json"_q, 4 * 1024 * 1024).bytes == state);
		CHECK(PendingLocal(f).bytes == pending.bytes);
		CHECK(f.outward == 0 && f.pendingPosts == 0);
		f.engine->cancel();
	}
	for (auto stale = 0; stale != 9; ++stale) {
		auto f = ExecutorFiles(true, SyncCarryMode::Switch, true);
		f.engine->open(f.start); Must(f.progress.command);
		const auto original = *f.progress.command;
		const auto bytes = CaptureSyncPrivateFile(f.root + u"/carry.json"_q, 4 * 1024 * 1024).bytes;
		auto observed = f.reply(); observed.sendIntent = SyncCarrySendIntent::Issued;
		observed.source.reset(); observed.outcome = SyncCarryOutcome::Unknown;
		if (stale == 0) ++observed.command.ticket;
		if (stale == 1) ++observed.command.run;
		if (stale == 2) ++observed.command.revision;
		if (stale == 3) observed.command.operation = QString(32, u'f');
		if (stale == 4) ++observed.command.origin.account.userId;
		if (stale == 5) observed.command.origin.login += u"other"_q;
		if (stale == 6) observed.command.source = observed.command.target;
		if (stale == 7) observed.command.action = SyncCarryJournalAction::ResolveAgain;
		if (stale == 8) observed.command.dispatch = true;
		f.engine->complete(observed);
		CHECK(f.progress.command && f.progress.command->ticket == original.ticket);
		CHECK(CaptureSyncPrivateFile(f.root + u"/carry.json"_q, 4 * 1024 * 1024).bytes == bytes);
		f.engine->cancel();
	}
}

void FixedReviewProbes() {
	std::puts("FIX1 discard window"); ReviewDiscardWindow(); ReviewDiscardRefusalsAndFailures();
	std::puts("FIX1 pending floor"); ReviewPendingFloor();
	std::puts("FIX2 unknown imported intent"); PendingUnknownObservation(); PendingUnknownStateControls();
	std::puts("FIX1 own-winner restart"); ReviewOwnWinner();
	std::puts("FIX1 definite cleanup gone"); ReviewCleanupGone();
	std::puts("FIX1 actual uploader constructor"); ReviewUploadConstructor();
}

}

int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv); std::setbuf(stdout, nullptr);
	auto worker = std::thread([] { FixedReviewProbes(); }); worker.join();
	std::printf("CARRY REVIEW FIX1 %d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}

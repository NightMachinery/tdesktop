#include "purple/purple_sync_auto_store.h"
#include "purple/purple_sync_auto_transport.h"
#include "purple/purple_sync_account_recovery.h"
#include "purple/purple_sync_config_apply.h"
#include "purple/purple_sync_config_review.h"
#include "purple/purple_sync_config_post.h"
#include "purple/purple_config.h"
#include "base/unixtime.h"
#include <QtCore/QUuid>
#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtCore/QSaveFile>
#include <QtCore/QDir>
#include <cstdio>
#include <limits>

namespace base { void LogWriteMain(const QString &) {} namespace assertion { void log(const char *, const char *, int) {} } }
namespace {
using namespace Purple;
auto Checks = 0;
auto Failures = 0;
DesktopAccountConfig *Config = nullptr;
AccountKey Account = { 0, 202 };
QByteArray Token;

void Check(bool value, const char *expression, int line) {
	++Checks;
	if (!value) { ++Failures; std::printf("FAIL auto:%d %s\n", line, expression); }
}
#define CHECK(value) Check(bool(value), #value, __LINE__)
void Put(const QString &path, const QByteArray &bytes) {
	CHECK(QDir().mkpath(QFileInfo(path).absolutePath()));
	auto file = QSaveFile(path);
	CHECK(file.open(QIODevice::WriteOnly));
	CHECK(file.write(bytes) == bytes.size());
	CHECK(file.commit());
}
}

namespace Purple {
uint64_t SyncConfigPostsStarted(not_null<Main::Session*>) { return 0; }
SyncConfigSendQueue SyncConfigSendQueueOf(not_null<Main::Session*>) { return SyncConfigSendQueue::Empty; }
std::optional<AccountKey> SettingsStartupOwner() { return AccountKey{ 0, 101 }; }
std::optional<AccountTransferReview> ReviewAccountTransfer(not_null<Main::Session*>, const QByteArray &bytes) {
	return Config ? Config->reviewTransfer(Account, bytes) : std::nullopt;
}
std::optional<AccountOperationTarget> AccountSyncInspectionTarget(not_null<Main::Session*>) { return Config->operationTarget(Account, false, true); }
std::optional<AccountOperationTarget> AccountSyncTarget(not_null<Main::Session*>) { return Config->operationTarget(Account); }
bool ValidateAccountSyncTarget(not_null<Main::Session*>, const AccountOperationTarget &target, bool same) { return Config->validateOperation(target, same); }
bool ValidateAccountSyncStoreTarget(not_null<Main::Session*>, const AccountOperationTarget &target, const SettingsFileInput &expected, bool same) { return Config->validateStore(target, expected, same); }
SyncHistoryAccess AccountSyncHistoryAccess(not_null<Main::Session*>, const AccountOperationTarget &target) { return { target.files, [target] { return Config->validateOperation(target); } }; }
std::optional<AccountTransferReview> RebaseAccountTransferAfterSetup(not_null<Main::Session*>, const AccountTransferReview &review) { return Config->rebaseTransferAfterSetup(review); }
AccountTransferResult ReplaceReviewedAccountSettings(not_null<Main::Session*>, const AccountTransferReview &review) { return Config->replaceReviewed(review, [] { return true; }); }
void NoteSettingsImported(not_null<Main::Session*>, const QByteArray &) {}
QByteArray AccountSyncBindingToken(Main::Account &) { return Token; }
bool SyncAccountAvailable(Main::Account &, Main::Session &, uint64_t id) { return id == uint64_t(Account.userId); }
SyncAccountBindingVerdict CheckAccountSyncBinding(const SyncLocalState &state, Main::Account &) { return CheckSyncAccountBinding(state, Token); }
SyncAccountSetupResult InitializeSyncAccountLocally(Main::Account &, Main::Session &, const SyncAccountInventoryResult &) { return { SyncAccountSetupStatus::NeedsReview }; }
std::optional<DesktopSyncRecovery> InspectSyncAccountRecovery(Main::Session &, const SyncAccountInventoryResult *) {
	auto result = DesktopSyncRecovery(); result.plan.status = SyncAccountRecoveryStatus::Bound; return result;
}
}

namespace {
const auto T0 = QByteArray("version=2\n[last_seen]\ntrade_p=false\n");
const auto T1 = QByteArray("version=2\n[last_seen]\ntrade_p=true\n");
const auto T2 = QByteArray("version=2\n[last_seen]\ntrade_p=true\nreasons_p=false\n");
ConfigVersion Version(const QByteArray &bytes) { return InspectConfigPayload(ParseSyncEnvelope(bytes)).version; }

struct Fixture {
	QTemporaryDir temp;
	std::unique_ptr<DesktopAccountConfig> config;
	AccountFiles files;
	AccountKey key = { 0, 202 };
	QByteArray token;
	std::unique_ptr<DesktopSyncAutoStore> store;
	SyncAutoFacts facts;
	std::map<int32_t, QByteArray> cloud;
	int32_t nextId = 100;
	int64_t now = 1800000000;
	bool current = true;
	bool strictContext = false;
	std::optional<AccountSettingsOpenTarget> opening;
	bool affirmAbsent = true;
	QString session = u"session-a"_q;
	std::optional<AccountTransferReview> factsReview;
	std::optional<AccountTransferReview> applyReview;

	Fixture() {
		Put(temp.filePath(u"settings.toml"_q), T0);
		config = std::make_unique<DesktopAccountConfig>(temp.path());
		config->start({ { { 0, 101 }, false, false }, { key, false, false } }, AccountKey{ 0, 101 });
		files = *config->files(key);
		Put(files.settings, T0);
		auto state = SyncLocalState();
		state.install = *FormatSyncInstallId(QByteArray(16, 'a'));
		state.createdDevice = u"desktop:"_q + state.install;
		state.space = *FormatSyncSpaceId(QByteArray(16, 's'));
		state.bindingToken = *FormatSyncBindingToken(QByteArray(16, 't'));
		token = state.bindingToken.toLatin1();
		{ auto local = SyncLocalStore(files.syncDirectory); CHECK(local.Open(true).status == SyncStoreStatus::Uninitialized); CHECK(local.Initialize(state)); }
		facts.account = facts.fileAccount = key;
		facts.signedIn = facts.ownerAnswered = facts.chatResolved = facts.chatWritable = true;
		facts.saveDebounceFinished = facts.impactContextComplete = true;
		facts.editSupported = false;
		facts.bindingToken = token;
		facts.writer = { u"macOS"_q, u"test"_q };
	}

	void Open() {
		opening = config->openTarget(key, SettingsOpenRole::Own);
		store = std::make_unique<DesktopSyncAutoStore>(files, key, token, [&] {
			const auto fresh = config->openTarget(key, SettingsOpenRole::Own);
			return current && (!strictContext || (opening && fresh && SameDesktopSyncAutoContext(*opening, *fresh)));
		});
		CHECK(store->Open());
	}
	void Restart() { store.reset(); session += u"-new"_q; Open(); }
	void Local(const QByteArray &bytes, bool saved = true) {
		Put(files.settings, bytes);
		facts.savedFingerprint = saved ? SettingsFingerprint(bytes) : QString();
	}
	SyncAutoFacts Facts() {
		auto result = facts;
		result.session = session;
		result.state = *store->local().state();
		result.currentDevice = result.state->createdDevice;
		result.local = ReadSyncSettingsFile(files);
		if (result.state->config.pendingSeq) { result.stagedRecord = store->local().ReadPendingConfig().staged; }
		factsReview = config->reviewTransfer(key, result.local.text);
		const auto &review = factsReview;
		if (review) { result.impactContext = review->context; result.proposedWorkMode = review->context.workMode; result.impactContextComplete = !review->usingGood; }
		return result;
	}
	SyncAutoInventory Snapshot() {
		auto scan = SyncHistoryScanResult(); scan.status = SyncHistoryScanStatus::Complete;
		auto read = SyncCandidateReadResult();
		auto snapshot = SyncAutoInventory();
		for (const auto &[id, bytes] : cloud) {
			scan.candidateIds.push_back(id);
			auto candidate = ClassifySyncCandidate(id, bytes);
			candidate.documentId = uint64_t(id) + 5000;
			candidate.editDate = uint64_t(now);
			read.records.push_back(candidate);
			snapshot.reads.push_back({ id, bytes, true, true, SyncAutoMessagePresence::Present });
		}
		if (affirmAbsent && store->record().retiring && !cloud.contains(store->record().messageId)) {
			snapshot.reads.push_back({ store->record().messageId, {}, true, false, SyncAutoMessagePresence::Absent });
		}
		read.status = AggregateSyncCandidateRead(read.records);
		snapshot.inventory = FinishSyncAccountInventory(uint64_t(key.userId), scan, read);
		snapshot.account = key;
		snapshot.session = session;
		snapshot.ticket = store->record().ticket;
		snapshot.readAt = now;
		snapshot.fresh = true;
		return snapshot;
	}
	SyncAutoPlan Event(SyncAutoEvent event, SyncAutoOutcome outcome = SyncAutoOutcome::Inventory, std::optional<SyncAutoInventory> snapshot = std::nullopt) {
		if (!store) { Open(); }
		const auto plan = store->Transition(Facts(), event, now, store->record().ticket, outcome, snapshot);
		CHECK(ReadDesktopSyncAuto(files, key).record == store->record());
		if (plan.action == SyncAutoAction::AutoApply) {
			applyReview = factsReview;
			if (applyReview) {
				applyReview->context.proposed = { plan.bytes, true };
				applyReview->impact = AnalyzeConfigChange(applyReview->context);
			}
		}
		return plan;
	}
	SyncAutoPlan Inventory() { return Event(SyncAutoEvent::Completed, SyncAutoOutcome::Inventory, Snapshot()); }
	bool Execute(const SyncAutoPlan &plan) {
		return store->ExecuteLocal(plan, [&](SyncLocalStore &local) {
			Config = config.get(); Account = key; Token = token;
			auto session = Main::Session(); auto account = Main::Account();
			const auto inventory = Snapshot().inventory;
			const auto review = ReviewSyncConfigInventory(inventory, local.state(), {}, ReadSyncSettingsFile(files));
			const auto replacement = applyReview;
			if (!replacement || !plan.apply || !plan.apply->source) { return false; }
			const auto result = ApplySyncConfigChoice(account, session, inventory, review,
				plan.apply->source->head.key, &*replacement, &local);
			return result.status == SyncConfigApplyStatus::Applied;
		});
	}
	void Seed(const QByteArray &text) {
		store.reset();
		auto local = SyncLocalStore(files.syncDirectory); CHECK(local.Open(true));
		const auto state = *local.state();
		const auto parents = cloud.empty() ? std::vector<ConfigVersion>() : std::vector<ConfigVersion>{ Version(cloud.rbegin()->second) };
		const auto built = BuildConfigRecord({ text, parents, std::nullopt, state.space,
			state.install, state.createdDevice, u"macOS"_q, u"test"_q, state.config.seq + 1, uint64_t(now) });
		CHECK(built);
		auto data = state.configData; data.pending = built.version.key;
		CHECK(local.StageConfig(built.canonical, data, token));
		CHECK(local.ConfirmConfigReadBack(built.canonical, state.createdDevice, nextId));
		cloud[nextId++] = built.canonical;
		Local(text);
	}
	void Remote(const QByteArray &text, bool successor = true) {
		const auto state = *store->local().state();
		const auto install = *FormatSyncInstallId(QByteArray(16, 'b'));
		const auto parents = successor ? std::vector<ConfigVersion>{ Version(cloud.rbegin()->second) } : std::vector<ConfigVersion>();
		const auto built = BuildConfigRecord({ text, parents, std::nullopt, state.space,
			install, u"android:"_q + install, u"Android"_q, u"test"_q, 1, uint64_t(now) });
		CHECK(built); cloud[nextId++] = built.canonical;
	}
};

SyncAutoPlan Stage(Fixture &f) {
	CHECK(f.Event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
	const auto plan = f.Inventory(); CHECK(plan.action == SyncAutoAction::Publish);
	CHECK(f.Execute(plan));
	CHECK(f.Event(SyncAutoEvent::Completed, SyncAutoOutcome::Staged).action == SyncAutoAction::Check);
	return f.Inventory();
}
SyncAutoPlan Confirm(Fixture &f, const SyncAutoPlan &send, bool edit = false) {
	CHECK(send.action == (edit ? SyncAutoAction::EditInPlace : SyncAutoAction::Post));
	if (edit) {
		const auto read = SyncAutoServerRead{ send.messageId, f.cloud[send.messageId], true, true, SyncAutoMessagePresence::Present };
		CHECK(f.store->AuthorizeEffect(send, &read));
	} else { CHECK(f.store->AuthorizeEffect(send)); }
	f.cloud[edit ? send.messageId : f.nextId++] = send.bytes;
	CHECK(f.Event(SyncAutoEvent::Completed, SyncAutoOutcome::Sent).action == SyncAutoAction::Reconcile);
	const auto commit = f.Inventory(); CHECK(commit.commitOnly); CHECK(f.Execute(commit));
	CHECK(f.Event(SyncAutoEvent::Completed, SyncAutoOutcome::Committed).action == SyncAutoAction::Check);
	return f.Inventory();
}

void PublishEditAndIsolation() {
	auto a = Fixture(); auto b = Fixture();
	const auto untouched = ReadSettingsFileInput(a.files.syncStore);
	CHECK(!ReadDesktopSyncAuto(b.files, b.key).record->enabled);
	b.Local(T1); const auto first = Stage(b); CHECK(first.action == SyncAutoAction::Post);
	CHECK(Confirm(b, first).action == SyncAutoAction::NoOp);
	CHECK(ReadSettingsFileInput(a.files.syncStore) == untouched);
	CHECK(ReadSettingsFileInput(a.files.settings).bytes == T0);
	CHECK(!QFileInfo::exists(a.files.syncDirectory + u"/auto.json"_q));
	b.facts.editSupported = true; b.Local(T2);
	CHECK(b.Event(SyncAutoEvent::LocalSaved).action == SyncAutoAction::Check);
	const auto stage = b.Inventory(); CHECK(b.Execute(stage));
	CHECK(b.Event(SyncAutoEvent::Completed, SyncAutoOutcome::Staged).action == SyncAutoAction::Check);
	const auto edit = b.Inventory(); CHECK(Confirm(b, edit, true).action == SyncAutoAction::NoOp);
	CHECK(b.store->local().state()->config.pendingSeq == 0);
}

void UncertainAndRestart() {
	for (const auto posted : { false, true }) {
		auto f = Fixture(); f.Local(T1); const auto send = Stage(f);
		if (posted) { f.cloud[f.nextId++] = send.bytes; }
		const auto retry = f.Event(SyncAutoEvent::Completed, SyncAutoOutcome::OutcomeUnknown);
		CHECK(retry.action == SyncAutoAction::RetryAt);
		const auto deadline = retry.retryAt;
		f.Restart();
		for (const auto trigger : { SyncAutoEvent::SessionReady, SyncAutoEvent::Foreground, SyncAutoEvent::Unlock, SyncAutoEvent::LocalSaved }) {
			const auto merged = f.Event(trigger); CHECK(merged.action == SyncAutoAction::NoOp);
			CHECK(merged.record.retryAt == deadline && merged.record.retries == 1);
		}
		f.now = deadline - 1; CHECK(f.Event(SyncAutoEvent::Retry).action == SyncAutoAction::NoOp);
		f.now = deadline; CHECK(f.Event(SyncAutoEvent::Retry).action == SyncAutoAction::Reconcile);
		const auto reconciled = f.Inventory();
		if (posted) { CHECK(reconciled.commitOnly); CHECK(f.Execute(reconciled)); CHECK(f.cloud.size() == 1); }
		else { CHECK(reconciled.action == SyncAutoAction::Post); CHECK(reconciled.bytes == send.bytes); }
	}
}

void BackoffRunBudget() {
	auto f = Fixture(); f.Open(); CHECK(f.Event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
	for (const auto delay : { 30, 120, 600 }) {
		const auto retry = f.Event(SyncAutoEvent::Completed, SyncAutoOutcome::NetworkFailure);
		CHECK(retry.action == SyncAutoAction::RetryAt && retry.retryAt == f.now + delay);
		const auto count = retry.record.retries; const auto run = retry.record.run;
		f.Event(SyncAutoEvent::Disable); f.Restart(); f.Event(SyncAutoEvent::Enable);
		CHECK(f.store->record().retryAt == retry.retryAt && f.store->record().retries == count && f.store->record().run == run);
		f.now = retry.retryAt; CHECK(f.Event(SyncAutoEvent::Retry).action == SyncAutoAction::Reconcile);
	}
	const auto stopped = f.Event(SyncAutoEvent::Completed, SyncAutoOutcome::NetworkFailure);
	CHECK(stopped.record.runStopped && stopped.reason == SyncAutoReason::RetryStopped);
	f.facts.ownerAnswered = false; CHECK(f.Event(SyncAutoEvent::Foreground).action == SyncAutoAction::Hold);
	f.Event(SyncAutoEvent::Disable); f.Restart(); f.Event(SyncAutoEvent::Enable);
	CHECK(f.store->record().runStopped);
	f.facts.ownerAnswered = true; const auto fresh = f.Event(SyncAutoEvent::Foreground);
	CHECK(fresh.action == SyncAutoAction::Reconcile && !fresh.record.runStopped && fresh.record.retries == 0 && fresh.record.run == stopped.record.run + 1);
}

void RetirementAndRefusal() {
	for (auto presence = 0; presence != 3; ++presence) {
		auto f = Fixture(); f.Seed(T0); f.Seed(T1); f.Open();
		CHECK(f.Event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
		const auto retire = f.Inventory(); CHECK(retire.action == SyncAutoAction::Retire);
		const auto unknown = SyncAutoServerRead{ retire.messageId, {}, true, false, SyncAutoMessagePresence::Unknown };
		CHECK(!f.store->CommitRetirement(retire, unknown));
		if (presence != 2) { f.cloud.erase(retire.messageId); }
		CHECK(f.Event(SyncAutoEvent::Completed, SyncAutoOutcome::OutcomeUnknown).action == SyncAutoAction::RetryAt);
		f.Restart(); f.now = f.store->record().retryAt; f.affirmAbsent = presence == 1;
		CHECK(f.Event(SyncAutoEvent::Retry).action == SyncAutoAction::Reconcile);
		const auto decision = f.Inventory();
		if (presence == 0) { CHECK(decision.action == SyncAutoAction::RetryAt); CHECK(f.store->local().state()->ownMessages.size() == 2); }
		if (presence == 1) { CHECK(decision.commitOnly); CHECK(f.Execute(decision)); CHECK(f.store->local().state()->ownMessages.size() == 1); }
		if (presence == 2) { CHECK(decision.action == SyncAutoAction::Retire); CHECK(f.store->local().state()->ownMessages.size() == 2); }
	}
}

void ApplyAndBoundaryHolds() {
	for (const auto &remote : { T1, QByteArray("version=1\n") }) {
		auto f = Fixture(); f.Seed(T0); f.Open(); f.Remote(remote);
		CHECK(f.Event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
		const auto plan = f.Inventory();
		if (remote == T1) {
			CHECK(plan.action == SyncAutoAction::AutoApply); CHECK(f.Execute(plan));
			CHECK(ReadSettingsFileInput(f.files.settings).bytes == T1);
			const auto target = *f.config->operationTarget(f.key, false, true);
			const auto history = SyncHistoryAccess{ f.files, [&] { return f.config->validateOperation(target); } };
			const auto entries = ListSyncConfigHistory(history); CHECK(!entries.empty());
			if (!entries.empty()) { CHECK(ReadSyncConfigHistory(history, entries.front().id) == T0); }
		} else { CHECK(plan.action == SyncAutoAction::Hold); CHECK(ReadSettingsFileInput(f.files.settings).bytes == T0); }
	}
	{
		auto f = Fixture(); f.Seed(T0 + "[peek]\nexit_hidden_chats_p=true\n"); f.Open(); f.Remote(T1);
		CHECK(f.Event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
		CHECK(f.Inventory().action == SyncAutoAction::Hold);
	}
	for (auto gate = 0; gate != 6; ++gate) {
		auto f = Fixture(); f.Open();
		if (gate == 0) { f.facts.ownerAnswered = false; }
		if (gate == 1) { f.facts.chatResolved = false; }
		if (gate == 2) { CHECK(QFile::remove(f.files.settings)); }
		if (gate == 3) { f.facts.fileAccount = { 1, f.key.userId }; }
		if (gate == 4) { Put(f.files.settings, QByteArray("broken=[\n")); Put(f.files.good, T0); }
		if (gate == 5) { Put(f.files.settings, QByteArray(kSyncSettingsMaximumBytes + 1, 'x')); }
		CHECK(f.Event(SyncAutoEvent::Enable).action == SyncAutoAction::Hold);
		CHECK(f.store->local().state()->config.seq == 0);
	}
}

void EditFallbackAndEffectRefusals() {
	auto f = Fixture(); f.Seed(T0); f.Open(); f.facts.editSupported = true; f.Local(T1);
	const auto edit = Stage(f); CHECK(edit.action == SyncAutoAction::EditInPlace);
	auto read = SyncAutoServerRead{ edit.messageId, f.cloud[edit.messageId], true, true, SyncAutoMessagePresence::Present };
	CHECK(f.store->AuthorizeEffect(edit, &read));
	read.owned = false; CHECK(!f.store->AuthorizeEffect(edit, &read)); read.owned = true;
	++read.messageId; CHECK(!f.store->AuthorizeEffect(edit, &read)); --read.messageId;
	read.record += ' '; CHECK(!f.store->AuthorizeEffect(edit, &read)); read.record.chop(1);
	f.current = false; CHECK(!f.store->AuthorizeEffect(edit, &read)); f.current = true;
	const auto reconcile = f.Event(SyncAutoEvent::Completed, SyncAutoOutcome::EditRefused);
	CHECK(reconcile.action == SyncAutoAction::Reconcile && !reconcile.commitOnly);
	CHECK(!f.store->AuthorizeEffect(edit, &read));
	const auto post = f.Inventory(); CHECK(post.action == SyncAutoAction::Post);
	const auto next = Confirm(f, post); CHECK(next.action == SyncAutoAction::Retire);
}

void RetirementLimitAndDeadline() {
	auto f = Fixture();
	for (auto i = 0; i != 7; ++i) { f.Seed(T0 + "# " + QByteArray::number(i) + "\n"); }
	f.Open(); CHECK(f.Event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
	auto plan = f.Inventory();
	for (auto i = 0; i != kSyncAutoRetireLimit; ++i) {
		CHECK(plan.action == SyncAutoAction::Retire);
		const auto present = SyncAutoServerRead{ plan.messageId, f.cloud[plan.messageId], true, true, SyncAutoMessagePresence::Present };
		CHECK(f.store->AuthorizeEffect(plan, &present));
		f.cloud.erase(plan.messageId);
		const auto absent = SyncAutoServerRead{ plan.messageId, {}, true, false, SyncAutoMessagePresence::Absent };
		CHECK(f.store->CommitRetirement(plan, absent));
		CHECK(f.Event(SyncAutoEvent::Completed, SyncAutoOutcome::Deleted).action == SyncAutoAction::Check);
		plan = f.Inventory();
	}
	CHECK(plan.action == SyncAutoAction::NoOp && f.store->local().state()->ownMessages.size() == 3);
	CHECK(f.Event(SyncAutoEvent::Foreground).action == SyncAutoAction::NoOp);
	f.now += kSyncAutoCheckSeconds - 1; CHECK(f.Event(SyncAutoEvent::Unlock).action == SyncAutoAction::NoOp);
	++f.now; CHECK(f.Event(SyncAutoEvent::Foreground).action == SyncAutoAction::Check);
}

void NeutralSaveProvenance() {
	auto f = Fixture();
	CHECK(!f.config->observeAccountSave(f.key, false));
	CHECK(!f.config->observeAccountSave(f.key));
	f.Local(T1);
	CHECK(f.config->observeAccountSave(f.key, false));
	CHECK(!f.config->observeAccountSave(f.key));
	CHECK(!f.config->observeAccountSave(f.key, false));
	const auto imported = f.config->reviewTransfer(f.key, T0); CHECK(imported);
	CHECK(f.config->importReviewed(*imported, [] { return true; }).ok);
	CHECK(!f.config->observeAccountSave(f.key, false));
	f.Local(T1); CHECK(f.config->observeAccountSave(f.key, false));
	f.Local(T0); CHECK(f.config->observeAccountSave(f.key, false));
	CHECK(QFile::remove(f.files.settings)); CHECK(!f.config->observeAccountSave(f.key, false));
	f.Local(T1); CHECK(!f.config->observeAccountSave(f.key, false));
	f.Local(T2); CHECK(f.config->observeAccountSave(f.key, false));
}

void CapturedSourceAndNativeChanges() {
	for (auto change = 0; change != 3; ++change) {
		auto f = Fixture(); f.strictContext = true; f.Local(T1); const auto post = Stage(f);
		CHECK(f.store->AuthorizeEffect(post));
		const auto before = ReadSettingsFileInput(f.files.syncStore);
		if (change == 0) {
			const auto choice = f.config->previewGlobalChoice(f.key, f.key, QString()); CHECK(choice);
			auto committed = f.config->commitGlobalChoice(*choice, [] { return true; }); CHECK(committed.ok);
			CHECK(f.config->activateGlobalChoice(*choice, committed));
		} else if (change == 1) {
			CHECK(f.config->writeWorkMode(*f.config->workModeTarget(f.key), AccountWorkMode::Follow, [] { return true; }).ok);
		} else {
			CHECK(QDir().rename(f.files.syncDirectory, f.files.syncDirectory + u"-old"_q));
			CHECK(QDir().mkpath(f.files.syncDirectory));
		}
		CHECK(!f.store->AuthorizeEffect(post));
		if (change != 2) { CHECK(ReadSettingsFileInput(f.files.syncStore) == before); }
		else {
			CHECK(QDir().rmdir(f.files.syncDirectory));
			CHECK(QDir().rename(f.files.syncDirectory + u"-old"_q, f.files.syncDirectory));
		}
		CHECK(ReadSettingsFileInput(f.temp.filePath(u"settings.toml"_q)).bytes == T0);
	}
}

void BusyBindingAndNoOp() {
	auto f = Fixture(); f.Open();
	{
		auto competitor = DesktopSyncAutoStore(f.files, f.key, f.token, [] { return true; });
		CHECK(!competitor.Open());
		CHECK(!QFileInfo::exists(f.files.syncDirectory + u"/auto.json"_q));
	}
	const auto first = f.Event(SyncAutoEvent::Enable);
	const auto older = first.ticket;
	const auto next = f.Inventory();
	const auto state = f.store->record();
	CHECK(f.store->Transition(f.Facts(), SyncAutoEvent::Completed, f.now, older - 1, SyncAutoOutcome::Sent).action == SyncAutoAction::NoOp);
	CHECK(f.store->record() == state);
	CHECK(!f.store->AuthorizeEffect(first));
	f.store.reset();
	CHECK(QDir(f.files.syncDirectory).rmdir(u"pending"_q));
	{
		auto local = SyncLocalStore(f.files.syncDirectory);
		CHECK(local.Open(true, QByteArray(32, 'f')).status == SyncStoreStatus::AccountUnbound);
		CHECK(!QFileInfo::exists(f.files.syncDirectory + u"/pending"_q));
	}
	CHECK(!next.record.active && next.record.retryAt == 0);
}

void SameRootAccountIsolation() {
	auto f = Fixture(); f.Open();
	const auto a = AccountKey{ 0, 101 };
	const auto files = *f.config->files(a);
	auto initial = SyncLocalState();
	initial.install = *FormatSyncInstallId(QByteArray(16, 'b'));
	initial.createdDevice = u"desktop:"_q + initial.install;
	initial.space = *FormatSyncSpaceId(QByteArray(16, 'r'));
	initial.bindingToken = *FormatSyncBindingToken(QByteArray(16, 'u'));
	{ auto local = SyncLocalStore(files.syncDirectory); CHECK(local.Open(true).status == SyncStoreStatus::Uninitialized); CHECK(local.Initialize(initial)); }
	auto own = DesktopSyncAutoStore(files, a, initial.bindingToken.toLatin1(), [] { return true; }); CHECK(own.Open());
	auto facts = f.Facts(); facts.account = facts.fileAccount = a;
	facts.state = initial; facts.bindingToken = initial.bindingToken.toLatin1(); facts.currentDevice = initial.createdDevice;
	facts.local = ReadSyncSettingsFile(files); facts.session = u"other-live-session"_q;
	CHECK(own.Transition(facts, SyncAutoEvent::Enable, f.now).action == SyncAutoAction::Check);
	const auto before = ReadSettingsFileInput(files.syncStore);
	const auto recordBefore = ReadDesktopSyncAuto(files, a).bytes;
	f.Local(T1); const auto post = Stage(f); CHECK(f.store->AuthorizeEffect(post));
	CHECK(ReadSettingsFileInput(files.settings).bytes == T0);
	CHECK(ReadSettingsFileInput(files.syncStore) == before);
	CHECK(ReadDesktopSyncAuto(files, a).bytes == recordBefore);
	CHECK(own.record().active && own.local().state()->config.pendingSeq == 0);
}

void OpeningApplyAndSavedBytes() {
	{
		auto f = Fixture(); f.Seed(T0); f.Open(); f.Remote(T1);
		CHECK(f.Event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
		const auto apply = f.Inventory(); CHECK(apply.action == SyncAutoAction::AutoApply);
		CHECK(f.config->writeWorkMode(*f.config->workModeTarget(f.key), AccountWorkMode::Follow, [] { return true; }).ok);
		CHECK(!f.Execute(apply));
		CHECK(ReadSettingsFileInput(f.files.settings).bytes == T0);
		CHECK(!QFileInfo::exists(f.files.backup));
	}
	{
		auto f = Fixture(); f.Local(T1);
		CHECK(f.Event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
		const auto publish = f.Inventory(); CHECK(publish.action == SyncAutoAction::Publish);
		f.Local(T2); CHECK(!f.Execute(publish));
		CHECK(f.store->local().state()->config.pendingSeq == 0 && f.store->local().state()->config.seq == 0);
		CHECK(ReadSettingsFileInput(f.files.settings).bytes == T2);
	}
}

void PersistedSavedIdentityAndDebounce() {
	{
		auto f = Fixture(); f.Local(T1); f.Open();
		f.facts.saveDebounceFinished = false;
		CHECK(f.Event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
		const auto plan = f.Inventory(); CHECK(plan.action == SyncAutoAction::Publish);
		CHECK(!f.Execute(plan)); CHECK(f.store->local().state()->config.seq == 0);
	}
	{
		auto f = Fixture(); f.Local(T1); f.Open();
		CHECK(f.Event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
		const auto plan = f.Inventory(); CHECK(plan.action == SyncAutoAction::Publish);
		const auto native = SyncPathIdentity(f.files.settings);
		Put(f.files.settings, T1);
		CHECK(SyncPathIdentity(f.files.settings) != native);
		CHECK(!f.Execute(plan)); CHECK(f.store->local().state()->config.seq == 0);
	}
	{
		auto f = Fixture(); f.Local(T1); f.Open();
		CHECK(f.Event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
		f.Inventory(); f.Restart(); f.facts.savedFingerprint.clear();
		CHECK(f.Event(SyncAutoEvent::SessionReady).action == SyncAutoAction::Reconcile);
		const auto plan = f.Inventory(); CHECK(plan.action == SyncAutoAction::Publish);
		CHECK(f.Execute(plan));
	}
}

void ConflictCloneFutureAndV2() {
	{
		auto f = Fixture(); f.Seed(T0); f.Local(T2); f.Open(); f.Remote(T1);
		CHECK(f.Event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
		CHECK(f.Inventory().reason == SyncAutoReason::Conflict);
		CHECK(ReadSettingsFileInput(f.files.settings).bytes == T2);
		CHECK(f.store->local().state()->config.pendingSeq == 0);
	}
	{
		auto f = Fixture(); f.Open(); const auto state = *f.store->local().state();
		const auto built = BuildConfigRecord({ T0, {}, std::nullopt, state.space, state.install,
			state.createdDevice, u"macOS"_q, u"test"_q, 4, uint64_t(f.now) }); CHECK(built);
		f.cloud[f.nextId++] = built.canonical;
		CHECK(f.Event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
		CHECK(f.Inventory().action == SyncAutoAction::Hold);
		CHECK(f.store->local().state()->config.seq == 0);
	}
	for (const auto &version : { QByteArray("3"), QByteArray("2147483648"), QByteArray("9223372036854775807") }) {
		auto f = Fixture(); f.Local("version=" + version + "\n[future]\npolicy='keep'\n"); Put(f.files.good, T0); f.Open();
		CHECK(f.Event(SyncAutoEvent::Enable).action == SyncAutoAction::Hold);
		CHECK(f.store->local().state()->config.seq == 0);
		CHECK(ReadSettingsFileInput(f.files.settings).bytes.contains(version));
	}
	for (const auto chat : { PeerToken(), PeerToken{ PeerTokenKind::Channel, 99 } }) {
		auto f = Fixture(); auto bytes = QByteArray();
		CHECK(ReadSyncPrivateFile(f.files.syncStore, 4 * 1024 * 1024, bytes) == SyncStoreStatus::Ready);
		auto parsed = ParseSyncLocalState(bytes); CHECK(parsed); parsed.state.version = 2; parsed.state.chat = chat;
		CHECK(WriteSyncPrivateFile(f.files.syncStore, SerializeSyncLocalState(parsed.state).canonical) == SyncStoreStatus::Ready);
		f.Open(); CHECK(f.Event(SyncAutoEvent::Enable).reason == SyncAutoReason::SyncChatNotResolved);
		CHECK(f.store->local().state()->config.seq == 0);
	}
	{
		auto f = Fixture(); f.Open(); f.facts.ownerAnswered = false;
		CHECK(f.Event(SyncAutoEvent::Enable).notice);
		CHECK(!f.Event(SyncAutoEvent::Foreground).notice);
		CHECK(f.store->record().noticed.size() == 1);
		f.facts.ownerAnswered = true; CHECK(QFile::remove(f.files.settings));
		CHECK(f.Event(SyncAutoEvent::Foreground).notice);
		CHECK(!QFileInfo::exists(f.files.settings)); CHECK(f.store->record().noticed.size() == 2);
	}
}

void ExactAbsenceBoundaries() {
	const auto make = [](QVector<MTPMessage> messages) {
		return MTPmessages_Messages(MTP_messages_messages(MTP_vector<MTPMessage>(messages),
			MTP_vector<MTPForumTopic>(), MTP_vector<MTPChat>(), MTP_vector<MTPUser>()));
	};
	const auto empty = MTPMessage(MTP_messageEmpty(MTP_flags(0), MTP_int(123), MTPPeer()));
	CHECK(InspectDesktopSyncExactMessage(make({}), 123, 202).presence == SyncAutoMessagePresence::Unknown);
	CHECK(InspectDesktopSyncExactMessage(make({ empty }), 124, 202).presence == SyncAutoMessagePresence::Unknown);
	CHECK(InspectDesktopSyncExactMessage(make({ empty, empty }), 123, 202).presence == SyncAutoMessagePresence::Unknown);
	CHECK(InspectDesktopSyncExactMessage(make({ empty }), 123, 202).presence == SyncAutoMessagePresence::Absent);
	const auto otherPeer = MTPMessage(MTP_messageEmpty(MTP_flags(MTPDmessageEmpty::Flag::f_peer_id), MTP_int(123), MTP_peerUser(MTP_long(303))));
	CHECK(InspectDesktopSyncExactMessage(make({ otherPeer }), 123, 202).presence == SyncAutoMessagePresence::Unknown);
}

struct ProbeEndpoint {
	bool isActive() const { return false; }
	void cancel() {}
	void reset() {}
};
struct ProbeChanges {
	int count = 0;
	void fire(std::nullptr_t) { ++count; }
} Changes;

class AutoSession {
public:
	bool Accept(const SyncAutoPlan &plan);
	std::optional<SyncAutoFacts> BuildFacts();
	bool Current() const { return _store && _store->Current(); }
	void Hold(SyncAutoReason reason);
	void Stop();
	void Complete(const SyncAutoPlan &plan, SyncAutoOutcome outcome,
		std::optional<SyncAutoInventory> inventory = std::nullopt);
	void InvokeInventory(const SyncAutoPlan &plan, SyncAccountInventoryResult inventory);
	struct Weak {
		AutoSession *value = nullptr;
		bool alive = true;
		AutoSession *get() const { return alive ? value : nullptr; }
	};
	struct SessionReference {
		Main::Session value;
		Main::Session *get() { return &value; }
	} _session;
	bool alive = true;
	bool _inFlight = true;
	QString _sessionId;
	SyncAutoPlan _plan;
	AccountKey _account;
	std::unique_ptr<DesktopSyncAutoStore> _store;
	std::optional<AccountSettingsOpenTarget> _opening;
	SyncAutoReason _failureReason = SyncAutoReason::None;
	ProbeEndpoint _retry, _scan, _exact, _post, _upload, _api, _save;
	QByteArray _token, _savedIdentity;
	QString _savedFingerprint;
	std::optional<AccountTransferReview> _factsReview;
	SyncConfigSendQueue _scanQueue = SyncConfigSendQueue::Empty;
	uint64_t _scanPosts = 0;
	SyncAccountInventoryResult _inventory;
	SyncAutoInventory _snapshot;
	std::vector<int32_t> _readIds;
	size_t _readIndex = 0;
	int factsBuilt = 0;
	int readNext = 0;
	int runs = 0;
	std::optional<SyncAutoFacts> Facts() { ++factsBuilt; return BuildFacts(); }
	void ReadNext() { ++readNext; }
	void Run(SyncAutoPlan) { ++runs; }

};

#include "auto-service-boundary.inc"

void FailedLocalCompletionCheckpoints() {
#ifdef Q_OS_UNIX
	constexpr auto writable = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
	constexpr auto readOnly = QFileDevice::ReadOwner | QFileDevice::ExeOwner;
	for (auto failure = 0; failure != 5; ++failure) {
		auto f = Fixture(); f.Local(T1); f.Open();
		CHECK(f.Event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
		auto plan = f.Inventory(); CHECK(plan.action == SyncAutoAction::Publish);
		if (failure >= 3) {
			CHECK(f.Execute(plan));
			CHECK(f.Event(SyncAutoEvent::Completed, SyncAutoOutcome::Staged).action == SyncAutoAction::Check);
			const auto send = f.Inventory(); CHECK(send.action == SyncAutoAction::Post);
			f.cloud[f.nextId++] = send.bytes;
			CHECK(f.Event(SyncAutoEvent::Completed, SyncAutoOutcome::Sent).action == SyncAutoAction::Reconcile);
			plan = f.Inventory(); CHECK(plan.commitOnly);
		}
		const auto checkpoint = f.store->record();
		const auto pending = f.files.syncDirectory + u"/pending"_q;
		if (failure == 0) { Put(pending + u"/config-1.json"_q, "inert collision"); }
		if (failure == 1) { CHECK(QDir(pending).removeRecursively()); }
		if (failure == 2 || failure == 3) { CHECK(QFile::setPermissions(f.files.syncDirectory, readOnly)); }
		if (failure == 4) { CHECK(QFile::setPermissions(pending, readOnly)); }
		CHECK(!f.Execute(plan));
		const auto status = f.store->local().status();
		CHECK(!f.store->local().state());
		CHECK(f.store->CheckCompletion(plan, f.session) == DesktopSyncAutoCompletion::LocalFailure);
		CHECK(QFile::setPermissions(f.files.syncDirectory, writable));
		if (failure == 4) { CHECK(QFile::setPermissions(pending, writable)); }
		const auto diskState = ReadSettingsFileInput(f.files.syncStore);
		auto session = AutoSession(); session._sessionId = f.session; session._plan = plan;
		session._store = std::move(f.store); session._account = f.key;
		const auto notices = Changes.count;
		CHECK(!session.BuildFacts());
		session.Complete(plan, SyncAutoOutcome::NetworkFailure);
		CHECK(session.factsBuilt == 0 && session.runs == 0);
		CHECK(!session._inFlight && !session._store);
		CHECK(session._failureReason == SyncAutoReason::StateChanged && Changes.count > notices);
		const auto held = ReadDesktopSyncAuto(f.files, f.key);
		CHECK(held.record && !held.record->active && held.record->held == SyncAutoReason::StateChanged);
		CHECK(held.record && held.record->ticket == checkpoint.ticket && held.record->session == checkpoint.session);
		CHECK(held.record && held.record->pending == checkpoint.pending && held.record->record == checkpoint.record);
		CHECK(held.record && held.record->expectedState == checkpoint.expectedState && held.record->expectedFile == checkpoint.expectedFile);
		CHECK(held.record && held.record->retries == checkpoint.retries && held.record->retryAt == checkpoint.retryAt
			&& held.record->run == checkpoint.run && held.record->runStopped == checkpoint.runStopped);
		CHECK(ReadSettingsFileInput(f.files.syncStore) == diskState);
		auto competitor = SyncLocalStore(f.files.syncDirectory);
		CHECK(competitor.Open(true).status != SyncStoreStatus::LockBusy);
		CHECK(failure != 0 || status == SyncStoreStatus::OrphanStage);
		CHECK(failure != 1 || status == SyncStoreStatus::IoError);
		CHECK((failure != 2 && failure != 3) || status == SyncStoreStatus::IoError);
		CHECK(failure != 4 || status == SyncStoreStatus::CleanupFailed);
		std::printf("LOCAL_FAILURE_FIXED scenario=%d status=%d facts=%d lock-released=1 checkpoint-held=1\n", failure, int(status), session.factsBuilt);
	}
#endif
}

void CurrentAndSupersededCallbacks() {
	for (auto scenario = 0; scenario != 6; ++scenario) {
		auto f = Fixture(); f.strictContext = true; f.Open();
		const auto plan = f.Event(SyncAutoEvent::Enable); CHECK(plan.action == SyncAutoAction::Check);
		const auto inventory = f.Snapshot().inventory;
		auto live = plan;
		if (scenario == 3) {
			const auto retry = f.Event(SyncAutoEvent::Completed, SyncAutoOutcome::NetworkFailure);
			CHECK(retry.action == SyncAutoAction::RetryAt);
			f.now = retry.retryAt;
			live = f.Event(SyncAutoEvent::Retry);
			CHECK(live.action == SyncAutoAction::Reconcile && live.ticket != plan.ticket);
		}
		auto session = AutoSession(); session._plan = live; session._sessionId = f.session;
		session._account = f.key; session._store = std::move(f.store);
		if (scenario == 1 || scenario >= 3) {
			CHECK(f.config->writeWorkMode(*f.config->workModeTarget(f.key), AccountWorkMode::Follow, [] { return true; }).ok);
		}
		if (scenario == 2) {
			const auto choice = f.config->previewGlobalChoice(f.key, f.key, QString()); CHECK(choice);
			auto committed = f.config->commitGlobalChoice(*choice, [] { return true; });
			CHECK(committed.ok); CHECK(f.config->activateGlobalChoice(*choice, committed));
		}
		auto callback = plan;
		if (scenario == 4) { callback.record.session = u"obsolete-owner"_q; }
		if (scenario == 5) { session.alive = false; }
		const auto notices = Changes.count;
		session.InvokeInventory(callback, inventory);
		CHECK(session.factsBuilt == 0);
		auto competitor = SyncLocalStore(f.files.syncDirectory);
		const auto status = competitor.Open(true).status;
		if (scenario == 1 || scenario == 2) {
			CHECK(!session._store && !session._inFlight && session.readNext == 0);
			CHECK(status == SyncStoreStatus::Ready && Changes.count > notices);
			CHECK(session._failureReason == SyncAutoReason::OwnershipChanged);
			const auto held = ReadDesktopSyncAuto(f.files, f.key);
			CHECK(held.record && !held.record->active && held.record->held == SyncAutoReason::OwnershipChanged);
			CHECK(held.record && held.record->ticket == plan.ticket && held.record->pending == plan.action);
		} else {
			CHECK(session._store && session._inFlight && status == SyncStoreStatus::LockBusy);
			CHECK(Changes.count == notices && session._failureReason == SyncAutoReason::None);
			CHECK(session.readNext == (scenario == 0 ? 1 : 0));
			CHECK(ReadDesktopSyncAuto(f.files, f.key).record == live.record);
		}
		std::printf("CALLBACK_FIXED scenario=%d read-next=%d lock-busy=%d in-flight=%d\n", scenario, session.readNext, status == SyncStoreStatus::LockBusy, session._inFlight);
	}
}

void PendingReadFailureDuringFacts() {
	auto f = Fixture(); f.Local(T1);
	const auto plan = Stage(f); CHECK(plan.action == SyncAutoAction::Post);
	const auto checkpoint = f.store->record();
	const auto pending = f.files.syncDirectory + u"/pending/config-1.json"_q;
	CHECK(QFile::remove(pending));
	CHECK(f.store->CheckCompletion(plan, f.session) == DesktopSyncAutoCompletion::Ready);
	auto session = AutoSession(); session._plan = plan; session._sessionId = f.session;
	session._store = std::move(f.store); session._account = f.key; session._token = f.token;
	session.Complete(plan, SyncAutoOutcome::NetworkFailure);
	CHECK(!session._store && !session._inFlight && session.factsBuilt == 1 && session.runs == 0);
	CHECK(session._failureReason == SyncAutoReason::StateChanged);
	const auto held = ReadDesktopSyncAuto(f.files, f.key);
	CHECK(held.record && !held.record->active && held.record->held == SyncAutoReason::StateChanged);
	CHECK(held.record && held.record->record == checkpoint.record && held.record->expectedState == checkpoint.expectedState);
	auto competitor = SyncLocalStore(f.files.syncDirectory);
	CHECK(competitor.Open(true).status != SyncStoreStatus::LockBusy);
}

void FailedHoldKeepsOpaqueCheckpoint() {
	auto f = Fixture(); f.Open();
	const auto plan = f.Event(SyncAutoEvent::Enable);
	auto session = AutoSession(); session._plan = plan; session._sessionId = f.session;
	session._store = std::move(f.store); session._account = f.key;
	const auto opaque = QByteArray("{\"version\":99}");
	const auto path = f.files.syncDirectory + u"/auto.json"_q;
	Put(path, opaque);
	const auto state = ReadSettingsFileInput(f.files.syncStore);
	const auto notices = Changes.count;
	session.Complete(plan, SyncAutoOutcome::NetworkFailure);
	CHECK(!session._store && !session._inFlight && session.factsBuilt == 0);
	CHECK(session._failureReason == SyncAutoReason::StateChanged && Changes.count > notices);
	CHECK(ReadSettingsFileInput(path).bytes == opaque);
	CHECK(ReadSettingsFileInput(f.files.syncStore) == state);
	auto competitor = SyncLocalStore(f.files.syncDirectory);
	CHECK(competitor.Open(true).status == SyncStoreStatus::Ready);
}

void CasAndOpaque() {
	auto f = Fixture(); f.Open(); f.Event(SyncAutoEvent::Enable);
	const auto before = ReadSettingsFileInput(f.files.syncStore);
	Put(f.files.syncDirectory + u"/auto.json"_q, QByteArray("{\"version\":99}"));
	CHECK(f.store->Transition(f.Facts(), SyncAutoEvent::Foreground, f.now).action == SyncAutoAction::Hold);
	CHECK(ReadSettingsFileInput(f.files.syncStore) == before);
	CHECK(!ReadDesktopSyncAuto(f.files, f.key).record);
	f.store.reset(); f.current = false;
	f.store = std::make_unique<DesktopSyncAutoStore>(f.files, f.key, f.token, [&] { return f.current; });
	CHECK(!f.store->Open());
}
}

int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv);
	PublishEditAndIsolation(); UncertainAndRestart(); BackoffRunBudget(); RetirementAndRefusal(); ApplyAndBoundaryHolds(); CasAndOpaque(); ExactAbsenceBoundaries(); EditFallbackAndEffectRefusals(); RetirementLimitAndDeadline(); NeutralSaveProvenance(); CapturedSourceAndNativeChanges(); BusyBindingAndNoOp(); SameRootAccountIsolation(); OpeningApplyAndSavedBytes(); PersistedSavedIdentityAndDebounce(); ConflictCloneFutureAndV2(); FailedLocalCompletionCheckpoints(); CurrentAndSupersededCallbacks(); FailedHoldKeepsOpaqueCheckpoint(); PendingReadFailureDuringFacts();
	std::printf("%d checks, %d failures\n", Checks, Failures);
	return Failures ? 1 : 0;
}

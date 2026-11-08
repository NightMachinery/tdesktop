#include "purple/purple_sync_auto_store.h"
#include "purple/purple_sync_auto_transport.h"
#include "purple/purple_sync_account_recovery.h"
#include "purple/purple_sync_config_apply.h"
#include "purple/purple_sync_config_review.h"
#include "purple/purple_sync_config_post.h"
#include "purple/purple_sync_magic_service.h"
#include "purple/purple_sync_magic_chat.h"
#include "purple/purple_config.h"
#include "base/unixtime.h"
#include <QtCore/QUuid>
#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtCore/QSaveFile>
#include <QtCore/QDir>
#include <cstdio>
#include <limits>
#include <thread>
#include <QtCore/QJsonArray>
#ifdef Q_OS_UNIX
#include <unistd.h>
#include <sys/stat.h>
#endif

#ifdef Q_OS_UNIX
std::function<void()> SyncDirectoryBeforeChmod;
extern "C" int SyncDirectoryChmodBoundary(int descriptor, mode_t mode) {
	if (const auto before = std::exchange(SyncDirectoryBeforeChmod, {})) { before(); }
	return ::fchmod(descriptor, mode);
}
#endif

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
uint64_t SyncConfigPostsStarted(not_null<Main::Session*>, PeerToken) { return 0; }
SyncConfigSendQueue SyncConfigSendQueueOf(not_null<Main::Session*>, PeerToken) { return SyncConfigSendQueue::Empty; }
bool MagicPaused = false;
bool SyncMagicPaused(not_null<Main::Session*>) { return MagicPaused; }
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
		snapshot.chat = store->local().state()->chat;
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
		const auto before = ReadSettingsFileInput(f.files.syncStore);
		const auto worker = SyncStoreWorkerThread();
		CHECK(f.store->local().CheckpointRetainedConfigBases()
			== (worker ? SyncStoreStatus::Ready : SyncStoreStatus::WrongThread));
		CHECK(f.store->local().state()->preserved.value(u"retained_config_bases"_q).toObject().value(u"records"_q).toArray().size() == (worker ? 1 : 2));
		if (!worker) {
			CHECK(ReadSettingsFileInput(f.files.syncStore) == before);
			CHECK(!QFileInfo::exists(f.files.syncDirectory + u"/retained-bases"_q));
		}
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
		f.Open();
		const auto plan = f.Event(SyncAutoEvent::Enable);
		CHECK(chat == PeerToken() ? plan.action == SyncAutoAction::Check : plan.reason == SyncAutoReason::SyncChatNotResolved);
		CHECK(f.store->local().state()->config.seq == 0);
		f.facts.chat = chat; f.facts.chatProblem = true;
		CHECK(f.Event(SyncAutoEvent::Foreground).reason == SyncAutoReason::SyncChatProblem);
		f.facts.chatProblem = false;
		CHECK(chat == PeerToken() || f.Event(SyncAutoEvent::Foreground).action == SyncAutoAction::Check);
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

DesktopSyncChatIo ProbeSelfRoute(AccountKey account) {
	auto facts = SyncChatFacts();
	facts.kind = SyncChatKind::Self;
	facts.canSendFiles = true;
	auto peer = DesktopSyncChatPeer{{account, u"auto-host-login"_q}, {}, SyncChatKind::Self};
	peer.facts = facts;
	return {std::move(peer), MTP_inputPeerSelf()};
}

bool SelfResolved = true;
bool SyncChatSelfResolved(Main::Session *) { return SelfResolved; }

void ExactAbsenceBoundaries() {
	const auto make = [](QVector<MTPMessage> messages) {
		return MTPmessages_Messages(MTP_messages_messages(MTP_vector<MTPMessage>(messages),
			MTP_vector<MTPForumTopic>(), MTP_vector<MTPChat>(), MTP_vector<MTPUser>()));
	};
	const auto empty = MTPMessage(MTP_messageEmpty(MTP_flags(MTPDmessageEmpty::Flag::f_peer_id),
		MTP_int(123), MTP_peerUser(MTP_long(202))));
	const auto peerless = MTPMessage(MTP_messageEmpty(MTP_flags(0), MTP_int(123), MTPPeer()));
	CHECK(InspectDesktopSyncExactMessage(make({ peerless }), 123, 202).presence == SyncAutoMessagePresence::Unknown);
	CHECK(InspectDesktopSyncExactMessage(make({}), 123, 202).presence == SyncAutoMessagePresence::Unknown);
	CHECK(InspectDesktopSyncExactMessage(make({ empty }), 124, 202).presence == SyncAutoMessagePresence::Unknown);
	CHECK(InspectDesktopSyncExactMessage(make({ empty, empty }), 123, 202).presence == SyncAutoMessagePresence::Unknown);
	CHECK(InspectDesktopSyncExactMessage(make({ empty }), 123, 202).presence == SyncAutoMessagePresence::Absent);
	const auto otherPeer = MTPMessage(MTP_messageEmpty(MTP_flags(MTPDmessageEmpty::Flag::f_peer_id), MTP_int(123), MTP_peerUser(MTP_long(303))));
	CHECK(InspectDesktopSyncExactMessage(make({ otherPeer }), 123, 202).presence == SyncAutoMessagePresence::Unknown);
}

struct ProbeEndpoint {
	int cancellations = 0;
	explicit operator bool() const { return true; }
	ProbeEndpoint *operator->() { return this; }
	void Cancel() { ++cancellations; }
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
	bool resolved = true;
	bool foreignSpace = false;
	bool Resolved() const { return resolved && _store && _store->local().state(); }
	PeerToken StateChat() const { return _store && _store->local().state() ? _store->local().state()->chat : PeerToken{}; }
	bool PauseOnForeignSpace(PeerToken) {
		if (foreignSpace) { Hold(SyncAutoReason::SyncChatProblem); }
		return foreignSpace;
	}
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
		CHECK(session._upload.cancellations == 1);
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
	for (auto scenario = 0; scenario != 7; ++scenario) {
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
		if (scenario == 1 || (scenario >= 3 && scenario != 6)) {
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
		if (scenario == 6) { session.foreignSpace = true; }
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
		} else if (scenario == 6) {
			CHECK(!session._store && !session._inFlight && session.readNext == 0);
			CHECK(status == SyncStoreStatus::Ready && Changes.count > notices);
			CHECK(session._failureReason == SyncAutoReason::SyncChatProblem);
		} else {
			CHECK(session._store && session._inFlight && status == SyncStoreStatus::LockBusy);
			CHECK(Changes.count == notices && session._failureReason == SyncAutoReason::None);
			CHECK(session.readNext == (scenario == 0 ? 1 : 0));
			CHECK(ReadDesktopSyncAuto(f.files, f.key).record == live.record);
		}
		std::printf("CALLBACK_FIXED scenario=%d read-next=%d lock-busy=%d in-flight=%d\n", scenario, session.readNext, status == SyncStoreStatus::LockBusy, session._inFlight);
	}
}

void MoveIntoBoundChat() {
	const auto chat = PeerToken{ PeerTokenKind::Channel, 77 };
	auto f = Fixture(); f.Local(T1);
	const auto plan = Stage(f); CHECK(plan.action == SyncAutoAction::Post);
	const auto stage = f.files.syncDirectory + u"/pending/config-1.json"_q;
	const auto staged = ReadSettingsFileInput(stage).bytes;
	CHECK(!staged.isEmpty() && f.store->local().state()->config.pendingSeq == 1);
	CHECK(!f.store->MoveTo(PeerToken{}, uint64_t(f.key.userId), SettingsFingerprint(T1)));
	CHECK(!f.store->MoveTo(chat, uint64_t(f.key.userId), QString()));
	CHECK(f.store->MoveTo(chat, uint64_t(f.key.userId), SettingsFingerprint(T1)));
	const auto moved = *f.store->local().state();
	CHECK(moved.chat == chat && moved.version == 2);
	CHECK(moved.config.pendingSeq == 0 && moved.config.abandonedSeq == 1);
	CHECK(!QFile::exists(stage));
	const auto record = ReadDesktopSyncAuto(f.files, f.key).record;
	CHECK(record && !record->active && record->queuedCheck && record->savedFingerprint == SettingsFingerprint(T1));
	CHECK(!f.store->MoveTo(chat, uint64_t(f.key.userId), SettingsFingerprint(T1)));
	Put(stage, staged);
	CHECK(QFile::setPermissions(stage, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
	f.Restart();
	const auto reopened = f.store->local().state();
	CHECK(reopened && reopened->chat == chat && !QFile::exists(stage));
	if (!reopened) { return; }
	f.facts.chat = chat;
	f.facts.savedFingerprint = SettingsFingerprint(T1);
	f.cloud.clear();
	CHECK(f.Event(SyncAutoEvent::Foreground).action == SyncAutoAction::Check);
	CHECK(f.Inventory().action == SyncAutoAction::Publish);
	f.current = false;
	CHECK(!f.store->MoveTo(PeerToken{}, uint64_t(f.key.userId), SettingsFingerprint(T1)));
	std::printf("MOVE_FIXED abandoned=%llu reopened=1 publish=1\n", (unsigned long long)moved.config.abandonedSeq);
}

void SetAsideForJoin() {
	auto f = Fixture(); f.Local(T1);
	CHECK(Stage(f).action == SyncAutoAction::Post);
	{
		auto busy = SyncLocalStore(f.files.syncDirectory);
		CHECK(busy.Open(true, f.token).status == SyncStoreStatus::LockBusy);
		CHECK(busy.SetAside() == SyncStoreStatus::InvalidTransition);
	}
	f.store.reset();
	auto local = std::make_unique<SyncLocalStore>(f.files.syncDirectory);
	CHECK(local->Open(true, f.token));
	CHECK(local->SetAside() == SyncStoreStatus::Ready);
	CHECK(!local->state() && local->SetAside() == SyncStoreStatus::InvalidTransition);
	const auto folders = QDir(f.files.syncDirectory).entryList({ u"orphaned-*"_q }, QDir::Dirs);
	CHECK(folders.size() == 1);
	const auto folder = f.files.syncDirectory + u'/' + folders.value(0);
	for (const auto name : { u"state.json"_q, u"auto.json"_q, u"pending/config-1.json"_q }) {
		CHECK(QFile::exists(folder + u'/' + name) && !QFile::exists(f.files.syncDirectory + u'/' + name));
	}
	CHECK(!ReadSyncRecoveryStore(f.files, f.key).store.statePresent);
	CHECK(SyncLocalStore(f.files.syncDirectory).Open(true, f.token).status == SyncStoreStatus::LockBusy);
	local.reset();
	CHECK(SyncLocalStore(f.files.syncDirectory).Open(true, f.token).status == SyncStoreStatus::AccountUnbound);
	std::printf("SET_ASIDE_FIXED folders=%d\n", int(folders.size()));
}

// The first automatic apply completes and the next pass stays clear with no Save and no post, also after a move.
void FirstAutomaticApplyStaysClear() {
	for (const auto moved : { false, true }) {
		auto f = Fixture(); f.Seed(T0); f.Open(); f.Remote(T1);
		if (moved) {
			const auto chat = PeerToken{ PeerTokenKind::Channel, 77 };
			f.cloud.erase(f.cloud.begin());
			CHECK(f.store->MoveTo(chat, uint64_t(f.key.userId), SettingsFingerprint(T0)));
			f.facts.chat = chat;
		}
		f.facts.savedFingerprint.clear();
		const auto posts = f.cloud.size();
		CHECK(f.Event(SyncAutoEvent::Enable).action == SyncAutoAction::Check);
		const auto apply = f.Inventory(); CHECK(apply.action == SyncAutoAction::AutoApply);
		CHECK(f.Execute(apply));
		CHECK(ReadSettingsFileInput(f.files.settings).bytes == T1);
		CHECK(f.Event(SyncAutoEvent::Completed, SyncAutoOutcome::Applied).action == SyncAutoAction::Check);
		const auto next = f.Inventory();
		CHECK(next.action == SyncAutoAction::NoOp && next.reason == SyncAutoReason::None);
		CHECK(f.store->record().held == SyncAutoReason::None && f.cloud.size() == posts);
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

SyncAccountInventoryResult CloudInventory(const Fixture &f) {
	auto scan = SyncHistoryScanResult(); scan.status = SyncHistoryScanStatus::Complete;
	auto read = SyncCandidateReadResult();
	for (const auto &[id, bytes] : f.cloud) {
		scan.candidateIds.push_back(id);
		auto candidate = ClassifySyncCandidate(id, bytes);
		candidate.documentId = uint64_t(id) + 5000; candidate.editDate = uint64_t(f.now);
		read.records.push_back(candidate);
	}
	read.status = AggregateSyncCandidateRead(read.records);
	return FinishSyncAccountInventory(uint64_t(f.key.userId), scan, read);
}

SyncAutoPlan Drive(Fixture &f, SyncAutoPlan plan) {
	for (auto step = 0; step != 16; ++step) {
		switch (plan.action) {
		case SyncAutoAction::Check:
			plan = f.Inventory();
			break;
		case SyncAutoAction::Reconcile:
			if (!plan.commitOnly) { plan = f.Inventory(); break; }
			CHECK(f.Execute(plan));
			plan = f.Event(SyncAutoEvent::Completed, SyncAutoOutcome::Committed);
			break;
		case SyncAutoAction::Publish:
			CHECK(f.Execute(plan));
			plan = f.Event(SyncAutoEvent::Completed, SyncAutoOutcome::Staged);
			break;
		case SyncAutoAction::AutoApply:
			CHECK(f.Execute(plan));
			plan = f.Event(SyncAutoEvent::Completed, SyncAutoOutcome::Applied);
			break;
		case SyncAutoAction::Post:
			CHECK(f.store->AuthorizeEffect(plan));
			f.cloud[f.nextId++] = plan.bytes;
			plan = f.Event(SyncAutoEvent::Completed, SyncAutoOutcome::Sent);
			break;
		default:
			return plan;
		}
	}
	CHECK(false);
	return plan;
}

void MakeSecondInstall(Fixture &b) {
	auto other = SyncLocalState();
	{
		auto local = SyncLocalStore(b.files.syncDirectory); CHECK(local.Open(true, b.token));
		if (local.state()) { other = *local.state(); }
	}
	other.install = *FormatSyncInstallId(QByteArray(16, 'c'));
	other.createdDevice = u"android:"_q + other.install;
	CHECK(QFile::remove(b.files.syncStore));
	{ auto local = SyncLocalStore(b.files.syncDirectory); CHECK(local.Open(true).status == SyncStoreStatus::Uninitialized); CHECK(local.Initialize(other)); }
}

void MoveFixture(Fixture &f, PeerToken chat, const QByteArray &settled) {
	CHECK(f.store->MoveTo(chat, uint64_t(f.key.userId), SettingsFingerprint(settled)));
	CHECK(f.store->local().state()->chat == chat);
	f.facts.chat = chat;
	f.facts.savedFingerprint = SettingsFingerprint(settled);
}

SyncAutoPlan LaterCheck(Fixture &f) {
	f.now += kSyncAutoCheckSeconds;
	return Drive(f, f.Event(SyncAutoEvent::Foreground));
}

void MigrationKeepsSharedSpace() {
	const auto magic = PeerToken{ PeerTokenKind::Channel, 88 };
	auto a = Fixture(); auto b = Fixture();
	MakeSecondInstall(b);
	a.Local(T1);
	CHECK(Drive(a, a.Event(SyncAutoEvent::Enable)).action == SyncAutoAction::NoOp);
	CHECK(a.cloud.size() == 1);
	b.cloud = a.cloud; b.nextId = a.nextId;
	b.Local(T1);
	CHECK(Drive(b, b.Event(SyncAutoEvent::Enable)).action == SyncAutoAction::NoOp);
	CHECK(b.cloud.size() == 1);
	const auto base = b.store->local().state()->configData.base;
	CHECK(!base.isEmpty() && base == a.store->local().state()->configData.base);
	const auto space = a.store->local().state()->space;
	CHECK(b.store->local().state()->space == space);
	for (const auto f : { &a, &b }) {
		MoveFixture(*f, magic, T1);
		CHECK(f->store->local().state()->space == space);
	}
	a.cloud.clear();
	CHECK(Drive(a, a.Event(SyncAutoEvent::Foreground)).action == SyncAutoAction::NoOp);
	CHECK(a.cloud.size() == 1);
	b.cloud = a.cloud; b.nextId = a.nextId;
	const auto settled = Drive(b, b.Event(SyncAutoEvent::Foreground));
	CHECK(settled.action == SyncAutoAction::NoOp && settled.reason != SyncAutoReason::Conflict);
	CHECK(b.cloud.size() == 1 && ReadSettingsFileInput(b.files.settings).bytes == T1);
	const auto review = ReviewSyncConfigInventory(b.Snapshot().inventory, b.store->local().state(), {},
		ReadSyncSettingsFile(b.files));
	CHECK(review.status == SyncConfigReviewStatus::Ready);
	CHECK(review.plan.verdict == ConfigSyncVerdict::UpToDate);
	CHECK(b.store->local().state()->space == space && a.store->local().state()->space == space);
	std::printf("MIGRATION_SHARED one_space=%d verdict=%d\n",
		int(b.store->local().state()->space == a.store->local().state()->space), int(review.plan.verdict));
}

// Sequential moves share one published base; racing moves each publish before they see it.
void MigrationBothPublished() {
	const auto magic = PeerToken{ PeerTokenKind::Channel, 89 };
	for (const auto race : { false, true }) {
		auto a = Fixture(); auto b = Fixture();
		MakeSecondInstall(b);
		a.Local(T1);
		CHECK(Drive(a, a.Event(SyncAutoEvent::Enable)).action == SyncAutoAction::NoOp);
		b.cloud = a.cloud; b.nextId = a.nextId;
		b.Local(T1);
		CHECK(Drive(b, b.Event(SyncAutoEvent::Enable)).action == SyncAutoAction::NoOp);
		b.Local(T2);
		CHECK(Drive(b, b.Event(SyncAutoEvent::LocalSaved)).action == SyncAutoAction::NoOp);
		a.cloud = b.cloud; a.nextId = b.nextId;
		CHECK(LaterCheck(a).action == SyncAutoAction::NoOp);
		CHECK(ReadSettingsFileInput(a.files.settings).bytes == T2);
		CHECK(a.store->local().state()->config.confirmedSeq && b.store->local().state()->config.confirmedSeq);
		const auto space = a.store->local().state()->space;
		MoveFixture(a, magic, T2);
		MoveFixture(b, magic, T2);
		a.cloud.clear(); b.cloud.clear();
		b.nextId = a.nextId + 50;
		CHECK(Drive(a, a.Event(SyncAutoEvent::Foreground)).action == SyncAutoAction::NoOp);
		if (!race) { b.cloud = a.cloud; }
		CHECK(Drive(b, b.Event(SyncAutoEvent::Foreground)).action == SyncAutoAction::NoOp);
		auto cloud = a.cloud;
		cloud.insert(b.cloud.begin(), b.cloud.end());
		CHECK(cloud.size() == (race ? 2 : 1));
		for (auto round = 0; round != 2; ++round) {
			for (const auto f : { &a, &b }) {
				f->cloud = cloud;
				const auto settled = LaterCheck(*f);
				CHECK(settled.action == SyncAutoAction::NoOp && settled.reason == SyncAutoReason::None);
				CHECK(f->cloud == cloud);
				CHECK(f->store->local().state()->space == space && !f->store->local().state()->config.pendingSeq);
				CHECK(ReadSettingsFileInput(f->files.settings).bytes == T2);
			}
		}
		std::printf("MIGRATION_BOTH_PUBLISHED race=%d records=%d\n", int(race), int(cloud.size()));
	}
}

// The device that never read R2 moves first: the other holds on Conflict, one prompt, with both files intact.
void MigrationBehindMovesFirst() {
	const auto magic = PeerToken{ PeerTokenKind::Channel, 90 };
	auto a = Fixture(); auto d = Fixture();
	MakeSecondInstall(a);
	d.Local(T1);
	CHECK(Drive(d, d.Event(SyncAutoEvent::Enable)).action == SyncAutoAction::NoOp);
	a.cloud = d.cloud; a.nextId = d.nextId;
	a.Local(T1);
	CHECK(Drive(a, a.Event(SyncAutoEvent::Enable)).action == SyncAutoAction::NoOp);
	a.Local(T2);
	CHECK(Drive(a, a.Event(SyncAutoEvent::LocalSaved)).action == SyncAutoAction::NoOp);
	const auto space = a.store->local().state()->space;
	MoveFixture(d, magic, T1);
	MoveFixture(a, magic, T2);
	d.cloud.clear();
	CHECK(Drive(d, d.Event(SyncAutoEvent::Foreground)).action == SyncAutoAction::NoOp);
	CHECK(d.cloud.size() == 1);
	a.cloud = d.cloud; a.nextId = d.nextId;
	const auto held = LaterCheck(a);
	std::printf("MIGRATION_BEHIND action=%d reason=%d records=%d\n", int(held.action), int(held.reason), int(a.cloud.size()));
	CHECK(held.action == SyncAutoAction::Hold && held.reason == SyncAutoReason::Conflict);
	CHECK(a.cloud == d.cloud && ReadSettingsFileInput(a.files.settings).bytes == T2 && ReadSettingsFileInput(d.files.settings).bytes == T1);
	CHECK(a.store->local().state()->space == space && d.store->local().state()->space == space);
}

// Every local commit after a post or a retirement names the chat the moved state lives in.
void MovedChatCommits() {
	const auto magic = PeerToken{ PeerTokenKind::Channel, 90 };
	for (const auto confirmed : { true, false }) {
		auto f = Fixture(); f.Local(T1);
		CHECK(Drive(f, f.Event(SyncAutoEvent::Enable)).action == SyncAutoAction::NoOp);
		MoveFixture(f, magic, T1);
		f.cloud.clear();
		CHECK(Drive(f, f.Event(SyncAutoEvent::Foreground)).action == SyncAutoAction::NoOp);
		f.Local(T2);
		const auto retire = Drive(f, f.Event(SyncAutoEvent::LocalSaved));
		CHECK(retire.action == SyncAutoAction::Retire);
		CHECK(f.store->local().state()->ownMessages.size() == 2);
		f.cloud.erase(retire.messageId);
		if (confirmed) {
			CHECK(f.store->CommitRetirement(retire, { retire.messageId, {}, true, false, SyncAutoMessagePresence::Absent }));
			CHECK(f.Event(SyncAutoEvent::Completed, SyncAutoOutcome::Deleted).action == SyncAutoAction::Check);
			CHECK(f.Inventory().action == SyncAutoAction::NoOp);
		} else {
			const auto retry = f.Event(SyncAutoEvent::Completed, SyncAutoOutcome::OutcomeUnknown);
			CHECK(retry.action == SyncAutoAction::RetryAt);
			f.now = retry.retryAt;
			CHECK(f.Event(SyncAutoEvent::Retry).action == SyncAutoAction::Reconcile);
			const auto commit = f.Inventory();
			CHECK(commit.commitOnly);
			CHECK(f.Execute(commit));
		}
		CHECK(f.store->local().state()->ownMessages.size() == 1);
		std::printf("MOVED_CHAT_COMMITS confirmed=%d chat=%d\n", int(confirmed), int(f.store->local().state()->chat == magic));
	}
}

void FirstUseLocalChoiceAppliesMatchingHead() {
	auto f = Fixture(); f.Open();
	const auto local = QByteArray("version = 2\n[last_seen]\ntrade_p = false\n");
	const auto other = QByteArray("version = 2\n[last_seen]\nreasons_p = false\n");
	f.Local(local);
	const auto state = *f.store->local().state();
	for (const auto writer : { 'b', 'c' }) {
		const auto install = *FormatSyncInstallId(QByteArray(16, writer));
		const auto record = BuildConfigRecord({ writer == 'b' ? local : other, {}, std::nullopt, state.space,
			install, u"desktop:"_q + install, u"macOS"_q, u"test"_q, 1, uint64_t(f.now) });
		CHECK(record); f.cloud[f.nextId++] = record.canonical;
	}
	f.store.reset();
	Config = f.config.get(); Account = f.key; Token = f.token;
	auto session = Main::Session(); auto account = Main::Account();
	const auto inventory = CloudInventory(f);
	auto input = SyncFirstUseInputOf(inventory, ReadSyncSettingsFile(f.files));
	CHECK(input && !input->headKey && input->expectedParents.size() == 2);
	if (!input) { return; }
	CHECK(PlanSyncFirstUse(*input) == SyncFirstUseAction::Ask);
	input->choice = SyncFirstUseChoice::ThisDevice;
	CHECK(PlanSyncFirstUse(*input) == SyncFirstUseAction::Publish);
	const auto review = ReviewSyncConfig(account, session, inventory, PeerToken{});
	const auto current = f.config->reviewTransfer(f.key, ReadSyncSettingsFile(f.files).text);
	CHECK(current);
	if (!current) { return; }
	const auto applied = ApplySyncConfigChoice(account, session, inventory, review, std::nullopt, &*current);
	CHECK(applied.status == SyncConfigApplyStatus::Applied && applied.adopted && applied.publishNeeded && !applied.wroteFile);
	CHECK(ReadSettingsFileInput(f.files.settings).bytes == local);
	CHECK(applied.expectedParents.size() == 2);
	auto store = SyncLocalStore(f.files.syncDirectory);
	CHECK(store.Open(true).status == SyncStoreStatus::Ready);
	if (!store.state()) { return; }
	const auto own = ReconcileOwnConfigInventory(*store.state(), inventory, uint64_t(f.key.userId), {});
	const auto gate = PlanSyncConfigPublishGate(*store.state(), inventory, own, applied.fingerprint,
		SyncConfigPublishRequest{ .expectedFingerprint = applied.fingerprint, .expectedParents = applied.expectedParents });
	CHECK(gate.status == SyncConfigPublishGateStatus::Proceed);
	std::printf("FIRST_USE_SPLIT_EXACT adopted=%d publish=%d parents=%d\n",
		int(applied.adopted), int(applied.publishNeeded), int(applied.expectedParents.size()));
}

void FirstUseJoinAppliesHead() {
	for (const auto local : { T0, QByteArray("version = 2\n") }) {
		for (const auto known : { true, false }) {
			auto f = Fixture(); f.Open(); f.Local(local);
			f.Remote(T2, false);
			f.store.reset();
			Config = f.config.get(); Account = f.key; Token = f.token;
			auto session = Main::Session(); auto account = Main::Account();
			const auto inventory = CloudInventory(f);
			const auto head = Version(f.cloud.begin()->second).key;
			const auto current = f.config->reviewTransfer(f.key, ReadSyncSettingsFile(f.files).text);
			CHECK(current);
			if (!current) { continue; }
			const auto result = ApplySyncFirstUseJoin(account, session, inventory,
				known ? std::optional(head) : std::nullopt, *current, PeerToken{});
			const auto defaults = (local != T0);
			CHECK((result.status == SyncConfigApplyStatus::Applied) == known);
			CHECK(ReadSettingsFileInput(f.files.settings).bytes == (known ? T2 : local));
			if (known) {
				CHECK(result.wroteFile && result.undoAvailable);
				CHECK(!QDir(f.files.history).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
			}
			std::printf("FIRST_USE_JOIN defaults=%d known=%d applied=%d\n", int(defaults), int(known),
				int(result.status == SyncConfigApplyStatus::Applied));
		}
	}
}

void FirstUseJoinAppliesCurrentHead() {
	for (const auto local : { QByteArray("version = 2\n"), T0 }) {
		auto f = Fixture(); f.Open(); f.Local(local);
		const auto state = *f.store->local().state();
		const auto installD = *FormatSyncInstallId(QByteArray(16, 'd'));
		const auto r1 = BuildConfigRecord({ T1, {}, std::nullopt, state.space,
			installD, u"desktop:"_q + installD, u"macOS"_q, u"test"_q, 1, uint64_t(f.now) });
		CHECK(r1); f.cloud[f.nextId++] = r1.canonical;
		f.Remote(T2, true);
		f.store.reset();
		Config = f.config.get(); Account = f.key; Token = f.token;
		auto session = Main::Session(); auto account = Main::Account();
		const auto inventory = CloudInventory(f);
		const auto input = SyncFirstUseInputOf(inventory, ReadSyncSettingsFile(f.files));
		const auto r2 = Version(f.cloud.rbegin()->second).key;
		CHECK(input && input->headKey == r2 && input->head);
		if (!input || !input->headKey) { continue; }
		const auto action = PlanSyncFirstUse(*input);
		CHECK(action == ((local == T0) ? SyncFirstUseAction::Ask : SyncFirstUseAction::Join));
		auto answered = *input; answered.choice = SyncFirstUseChoice::Synced;
		CHECK(PlanSyncFirstUse(answered) == SyncFirstUseAction::Join);
		const auto current = f.config->reviewTransfer(f.key, ReadSyncSettingsFile(f.files).text);
		CHECK(current);
		if (!current) { continue; }
		const auto result = ApplySyncFirstUseJoin(account, session, inventory, input->headKey, *current, PeerToken{});
		CHECK(result.status == SyncConfigApplyStatus::Applied);
		CHECK(ReadSettingsFileInput(f.files.settings).bytes == T2);
		CHECK(result.wroteFile && result.undoAvailable);
		std::printf("FIRST_USE_CURRENT_HEAD custom=%d action=%d applied=%d\n", int(local == T0),
			int(action), int(result.status == SyncConfigApplyStatus::Applied));
	}
	auto same = Fixture(); same.Open(); same.Local(T2);
	const auto state = *same.store->local().state();
	const auto installD = *FormatSyncInstallId(QByteArray(16, 'd'));
	const auto r1 = BuildConfigRecord({ T1, {}, std::nullopt, state.space,
		installD, u"desktop:"_q + installD, u"macOS"_q, u"test"_q, 1, uint64_t(same.now) });
	CHECK(r1); same.cloud[same.nextId++] = r1.canonical;
	same.Remote(T2, true);
	same.store.reset();
	Config = same.config.get(); Account = same.key; Token = same.token;
	auto session = Main::Session(); auto account = Main::Account();
	const auto inventory = CloudInventory(same);
	const auto input = SyncFirstUseInputOf(inventory, ReadSyncSettingsFile(same.files));
	CHECK(input && input->headKey == Version(same.cloud.rbegin()->second).key);
	CHECK(input && PlanSyncFirstUse(*input) == SyncFirstUseAction::JoinSilently);
	const auto current = same.config->reviewTransfer(same.key, ReadSyncSettingsFile(same.files).text);
	CHECK(current);
	if (input && current) {
		const auto result = ApplySyncFirstUseJoin(account, session, inventory, input->headKey, *current, PeerToken{});
		CHECK(result.status == SyncConfigApplyStatus::Applied && !result.wroteFile);
		CHECK(ReadSettingsFileInput(same.files.settings).bytes == T2);
	}
}

class BindingFilePort final : public std::enable_shared_from_this<BindingFilePort> {
public:
	explicit BindingFilePort(QString path) : _path(std::move(path)) {}
	bool current() const { return live; }
	bool live = true;
	QString _path;

#include "binding-port-boundary.inc"

};

void FreshLoginExistingChat() {
	for (const auto rootOwner : { true, false }) {
		for (const auto legacyCache : { false, true }) {
			for (const auto &text : { T0, T2 }) {
				auto temp = QTemporaryDir();
				Put(temp.filePath(u"settings.toml"_q), text);
				auto config = DesktopAccountConfig(temp.path());
				const auto owner = AccountKey{ 0, 101 };
				const auto key = rootOwner ? owner : AccountKey{ 0, 202 };
				config.start({ { owner, false, false }, { { 0, 202 }, false, false } }, owner);
				const auto files = *config.files(key);
				Put(files.settings, text);
				const auto path = files.syncDirectory + u"/chat.json"_q;
				const auto chat = PeerToken{ PeerTokenKind::Channel, 42 };
				auto binding = SyncChatBinding();
				binding.mode = SyncChatBindingMode::Magic;
				binding.chat = chat;
				const auto encoded = SerializeSyncChatBinding(binding);
				CHECK(encoded);
				if (!encoded) { continue; }
				if (legacyCache) {
					Put(path, *encoded);
#ifdef Q_OS_UNIX
					CHECK(QFile::setPermissions(files.syncDirectory, QFileDevice::ReadOwner | QFileDevice::WriteOwner
						| QFileDevice::ExeOwner | QFileDevice::ReadGroup | QFileDevice::ExeGroup
						| QFileDevice::ReadOther | QFileDevice::ExeOther));
					CHECK(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner
						| QFileDevice::ReadGroup | QFileDevice::ReadOther));
#endif
				}
				const auto port = std::make_shared<BindingFilePort>(path);
				const auto before = port->readBinding();
				CHECK(before && *before == (legacyCache ? *encoded : QByteArray()));
				CHECK(before && port->writeBinding(*before, *encoded));
				auto cached = QByteArray();
				CHECK(ReadSyncPrivateFile(path, kSyncMagicBindingLimit, cached) == SyncStoreStatus::Ready);
				CHECK(cached == *encoded);
				const auto snapshot = ReadSyncRecoveryStore(files, key);
				CHECK(snapshot.status == SyncStoreStatus::Ready);
				CHECK(!snapshot.store.statePresent && !snapshot.store.pendingPresent);
				std::printf("FRESH_LOGIN_CACHE owner=%d legacy=%d custom=%d read=%d\n", int(rootOwner),
					int(legacyCache), int(text == T0), int(snapshot.status));
				if (snapshot.status != SyncStoreStatus::Ready) { continue; }
				CHECK(SetDesktopSyncAutoBeforeState(files, key, true));
				CHECK(ReadDesktopSyncAuto(files, key).record->enabled);
				const auto space = *FormatSyncSpaceId(QByteArray(16, 's'));
				const auto writer = *FormatSyncInstallId(QByteArray(16, 'r'));
				const auto remote = BuildConfigRecord({ T2, {}, std::nullopt, space, writer,
					u"android:"_q + writer, u"Android"_q, u"test"_q, 1, 1800000000 });
				CHECK(remote);
				auto scan = SyncHistoryScanResult();
				scan.status = SyncHistoryScanStatus::Complete;
				scan.candidateIds = { 77 };
				auto read = SyncCandidateReadResult();
				auto candidate = ClassifySyncCandidate(77, remote.canonical);
				candidate.documentId = 5077;
				candidate.editDate = 1800000000;
				read.records = { candidate };
				read.status = AggregateSyncCandidateRead(read.records);
				const auto inventory = FinishSyncAccountInventory(uint64_t(key.userId), scan, read);
				CHECK(inventory.status == SyncAccountInventoryStatus::Complete);
				const auto first = ReviewSyncFirstUse(files, inventory);
				CHECK(first.need == SyncFirstUseNeed::NoState);
				CHECK(first.action == ((text == T2) ? SyncFirstUseAction::JoinSilently : SyncFirstUseAction::Ask));
				auto mark = SyncFirstUseMark();
				mark.choice = SyncFirstUseChoice::Synced;
				mark.joinSpace = space;
				mark.heads = { remote.version.key };
				CHECK(WriteSyncFirstUseMark(files, mark));
				auto state = SyncLocalState();
				state.install = *FormatSyncInstallId(QByteArray(16, 'n'));
				state.createdDevice = u"desktop:"_q + state.install;
				state.space = space;
				state.version = 2;
				state.chat = chat;
				state.bindingToken = *FormatSyncBindingToken(QByteArray(16, 't'));
				{
					auto store = SyncLocalStore(files.syncDirectory);
					CHECK(store.Open(true).status == SyncStoreStatus::Uninitialized);
					const auto initialized = store.Initialize(state);
					CHECK(initialized);
					if (!initialized) { continue; }
				}
				Config = &config; Account = key; Token = state.bindingToken.toLatin1();
				auto session = Main::Session(); auto account = Main::Account();
				const auto current = config.reviewTransfer(key, text);
				CHECK(current);
				if (!current) { continue; }
				const auto joined = ApplySyncFirstUseJoin(account, session, inventory,
					remote.version.key, *current, chat);
				CHECK(joined.status == SyncConfigApplyStatus::Applied);
				CHECK(ReadSettingsFileInput(files.settings).bytes == T2);
				auto automatic = DesktopSyncAutoStore(files, key, Token, [] { return true; });
				const auto opened = automatic.Open();
				CHECK(opened && automatic.record().enabled);
				if (!opened) { continue; }
				CHECK(automatic.local().state()->configData.base == remote.version.key);
			}
		}
	}
}

void RefuseRacedCacheDirectory() {
#ifdef Q_OS_UNIX
	auto temp = QTemporaryDir();
	const auto path = temp.filePath(u"sync"_q);
	const auto parked = temp.filePath(u"parked"_q);
	const auto outside = temp.filePath(u"outside"_q);
	CHECK(QDir().mkpath(outside));
	Put(path + u"/chat.json"_q, "inert-cache");
	CHECK(::chmod(QFile::encodeName(path).constData(), 0755) == 0);
	CHECK(::chmod(QFile::encodeName(outside).constData(), 0500) == 0);
	const auto before = QFileInfo(outside).permissions();
	auto raced = false;
	SyncDirectoryBeforeChmod = [&] {
		raced = true;
		CHECK(QDir().rename(path, parked));
		CHECK(QFile::link(outside, path));
	};
	CHECK(EnsureSyncPrivateDirectory(path) == SyncStoreStatus::IoError);
	SyncDirectoryBeforeChmod = {};
	CHECK(raced && QFileInfo(path).isSymLink());
	CHECK(QFileInfo(outside).permissions() == before);
	CHECK(ReadSettingsFileInput(parked + u"/chat.json"_q).bytes == "inert-cache");
#endif
}

void RefuseUnsafeCacheDirectories() {
#ifdef Q_OS_UNIX
	for (const auto kind : { 0, 1, 2, 3, 4 }) {
		auto temp = QTemporaryDir();
		const auto directory = temp.filePath(u"sync"_q);
		const auto outside = temp.filePath(u"outside"_q);
		const auto cache = directory + u"/chat.json"_q;
		if (kind == 4) {
			CHECK(QDir().mkpath(outside));
			CHECK(QFile::link(outside, directory));
		} else {
			CHECK(QDir().mkpath(directory));
		}
		CHECK(QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner
			| QFileDevice::ExeOwner | QFileDevice::ReadGroup | QFileDevice::ExeGroup
			| QFileDevice::ReadOther | QFileDevice::ExeOther
			| ((kind == 1) ? QFileDevice::WriteOther : QFileDevice::Permissions())));
		if (kind == 0) {
			Put(directory + u"/state.json"_q, "opaque-state-sentinel");
		} else if (kind == 2 || kind == 3) {
			Put(outside, "outside-sentinel");
			if (kind == 2) {
				CHECK(QFile::link(outside, cache));
			} else {
				CHECK(::link(QFile::encodeName(outside).constData(), QFile::encodeName(cache).constData()) == 0);
			}
		}
		const auto permissions = QFileInfo(directory).permissions();
		const auto port = std::make_shared<BindingFilePort>(cache);
		CHECK(!port->readBinding());
		CHECK(!port->writeBinding({}, "inert-cache"));
		CHECK(QFileInfo(directory).permissions() == permissions);
		if (kind == 0) {
			CHECK(ReadSettingsFileInput(directory + u"/state.json"_q).bytes == "opaque-state-sentinel");
		} else if (kind == 2 || kind == 3) {
			CHECK(ReadSettingsFileInput(outside).bytes == "outside-sentinel");
		}
	}
#endif
}

void AutoBeforeState() {
	auto temp = QTemporaryDir();
	Put(temp.filePath(u"settings.toml"_q), T0);
	auto config = DesktopAccountConfig(temp.path());
	const auto key = AccountKey{ 0, 202 };
	config.start({ { { 0, 101 }, false, false }, { key, false, false } }, AccountKey{ 0, 101 });
	const auto files = *config.files(key);
	Put(files.settings, T0);
	CHECK(SetDesktopSyncAutoBeforeState(files, key, true));
	CHECK(ReadDesktopSyncAuto(files, key).record->enabled);
	CHECK(SyncPathIdentity(files.syncStore) == "missing");
	CHECK(SetDesktopSyncAutoBeforeState(files, key, false));
	CHECK(!ReadDesktopSyncAuto(files, key).record->enabled);
	CHECK(SetDesktopSyncAutoBeforeState(files, key, true));
	auto state = SyncLocalState();
	state.install = *FormatSyncInstallId(QByteArray(16, 'a'));
	state.createdDevice = u"desktop:"_q + state.install;
	state.space = *FormatSyncSpaceId(QByteArray(16, 's'));
	state.bindingToken = *FormatSyncBindingToken(QByteArray(16, 't'));
	{ auto local = SyncLocalStore(files.syncDirectory); CHECK(local.Open(true).status == SyncStoreStatus::Uninitialized); CHECK(local.Initialize(state)); }
	CHECK(!SetDesktopSyncAutoBeforeState(files, key, false));
	auto store = DesktopSyncAutoStore(files, key, state.bindingToken.toLatin1(), [] { return true; });
	CHECK(store.Open() && store.record().enabled);
	std::printf("AUTO_BEFORE_STATE enabled=%d\n", int(store.record().enabled));
}

}

int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv);
	const auto run = [] {
	FirstAutomaticApplyStaysClear(); MigrationBehindMovesFirst(); PublishEditAndIsolation(); UncertainAndRestart(); BackoffRunBudget(); RetirementAndRefusal(); ApplyAndBoundaryHolds(); CasAndOpaque(); ExactAbsenceBoundaries(); EditFallbackAndEffectRefusals(); RetirementLimitAndDeadline(); NeutralSaveProvenance(); CapturedSourceAndNativeChanges(); BusyBindingAndNoOp(); SameRootAccountIsolation(); OpeningApplyAndSavedBytes(); PersistedSavedIdentityAndDebounce(); ConflictCloneFutureAndV2(); FailedLocalCompletionCheckpoints(); CurrentAndSupersededCallbacks(); FailedHoldKeepsOpaqueCheckpoint(); PendingReadFailureDuringFacts(); MoveIntoBoundChat(); SetAsideForJoin(); FirstUseLocalChoiceAppliesMatchingHead(); FirstUseJoinAppliesHead(); FirstUseJoinAppliesCurrentHead(); AutoBeforeState(); FreshLoginExistingChat(); RefuseRacedCacheDirectory(); RefuseUnsafeCacheDirectories(); MigrationKeepsSharedSpace(); MigrationBothPublished(); MovedChatCommits();
	};
	if (argc > 1 && QByteArray(argv[1]) == "--worker") {
		auto worker = std::thread(run);
		worker.join();
	} else {
		CHECK(!SyncStoreWorkerThread());
		run();
	}
	std::printf("%s: %d checks, %d failures\n",
		(argc > 1) ? "worker" : "application-main", Checks, Failures);
	return Failures ? 1 : 0;
}

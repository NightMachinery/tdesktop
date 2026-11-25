#include "purple/purple_archive_journal.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "purple/purple_archive_files.h"
#include "purple/purple_archive_envelope.h"
#include "tests/archive_test_support.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include <QtCore/QJsonDocument>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QLockFile>
#include "purple/purple_archive_attention.h"
#include <future>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <set>
#include <chrono>
#include <dlfcn.h>
#include "purple/purple_archive_sql_maintenance.h"

using namespace Purple;
using namespace ArchiveTest;
std::mutex GateMutex;
std::condition_variable GateCv;
std::atomic<bool> GateArmed = false, GateEntered = false, GateReleased = false;
extern "C" int fsync(int descriptor) {
	using Call = int(*)(int);
	static const auto actual = reinterpret_cast<Call>(dlsym(RTLD_NEXT, "fsync"));
	if (GateArmed.exchange(false)) {
		auto lock = std::unique_lock(GateMutex);
		GateEntered = true;
		GateCv.notify_all();
		GateCv.wait(lock, [] { return GateReleased.load(); });
	}
	return actual(descriptor);
}
struct ProbeSession : base::has_weak_ptr { AccountKey account{0, 123}; };
struct ProbeWorker {
	using Task = std::function<void(DesktopArchiveStore&)>;
	struct QueuedTask {
		std::function<void(DesktopArchiveStore &,
			const std::shared_ptr<DesktopArchiveJournal> &)> run;
		uint64_t startupBytes = 0;
	};
	struct State {
		Fn<void(DesktopArchiveStatus, QString, uint64_t)> report
			= [](DesktopArchiveStatus, QString, uint64_t) {};
		std::mutex mutex;
		std::condition_variable condition;
		std::deque<QueuedTask> tasks;
		size_t queuedStreams = 0;
		uint64_t startupBytes = 0;
		std::atomic<int> startupCount = 0;
		std::atomic<bool> startupFailed = false;
		std::atomic<bool> startupHeld = false;
		std::atomic<bool> purgeRequested = false;
		bool cancelled = false;
	};
	std::shared_ptr<State> _state = std::make_shared<State>();
	bool Post(Task task, bool cleanup = false);
	bool PostStartupCapture(ArchiveEvent event, int byteLimit);
	bool NeedsStartupCapture() const { return _state->startupCount.load() != 0; }
	bool StartupFailed() const { return _state->startupFailed.load(); }
	bool StartupHeld() const { return _state->startupHeld.load(); }
	std::shared_ptr<DesktopArchiveJournal> initialJournal;
	void Run(DesktopArchiveStore &store) {
		for (;;) {
			auto task = QueuedTask();
			{
				const auto lock = std::lock_guard(_state->mutex);
				if (_state->tasks.empty()) return;
				task = std::move(_state->tasks.front());
				_state->tasks.pop_front();

			}
			task.run(store, initialJournal);
			if (task.startupBytes) {
				const auto lock = std::lock_guard(_state->mutex);
				_state->startupBytes -= task.startupBytes;
				--_state->startupCount;
			}
		}
	}
};
#include "archive-worker-post.inc"
#include "archive-worker-startup.inc"
namespace Purple {
#include "archive-native-worker.inc"
}
struct ProbeValue {
	std::shared_ptr<DesktopArchiveJournal> journal;
	std::shared_ptr<ProbeWorker> worker;
	QString failure;
};
ProbeValue Value;
std::set<ProbeSession*> Retired;
AccountKey SessionAccountKey(not_null<ProbeSession*> session) { return session->account; }
ProbeValue &For(not_null<ProbeSession*>) { return Value; }
std::atomic<int> FallbackLosses = 0;
void ReportArchiveCaptureLoss(ProbeSession*, const DesktopArchiveCaptureLosses &losses) {
	for (const auto &[reason, count] : losses) {
		FallbackLosses += int(count);
	}
}
void CaptureOnMain(Fn<void()> callback) { callback(); }
#include "archive-failure-toast.inc"
void QueueArchiveRetentionGap(not_null<ProbeSession*> session, QString reason);
#include "archive-capture-queue.inc"
struct NativeProbeValue {
	std::shared_ptr<DesktopArchiveJournal> journal;
	std::shared_ptr<NativeStartupWorker> worker;
	QString failure;
};
NativeProbeValue NativeValue;
NativeProbeValue &NativeFor(not_null<ProbeSession*>) { return NativeValue; }
void NativeQueueArchiveRetentionGap(not_null<ProbeSession*> session, QString reason);
#include "archive-native-capture-queue.inc"
void StartupOnMain(Fn<void()> call) { call(); }
#include "archive-startup-purge.inc"


void HistoricalGapNotice() {
	Section = "historical gaps stay visible without repeating the startup toast";
	const auto history = u"capture-gap"_q;
	const auto current = u"outbox-append"_q;
	CHECK(!ArchiveFailureNeedsToast(DesktopArchiveStatus::Ready, history, {}));
	CHECK(!ArchiveFailureNeedsToast(DesktopArchiveStatus::Ready, {}, history));
	CHECK(!ArchiveFailureNeedsToast(DesktopArchiveStatus::RetentionGap, current, history));
	CHECK(!ArchiveFailureNeedsToast(DesktopArchiveStatus::Ready, current, history));
	CHECK(ArchiveFailureNeedsToast(DesktopArchiveStatus::Unreadable, current, history));
	CHECK(!ArchiveFailureNeedsToast(DesktopArchiveStatus::Unreadable, current, current));
	CHECK(!ArchiveFailureNeedsToast(DesktopArchiveStatus::Maintenance, current, {}));
}

void Case(bool failed) {
	Section = failed ? "foreground behind error attention fsync" : "foreground behind lifecycle fsync";
	auto temp = QTemporaryDir();
	CHECK(DesktopArchiveDirectory(temp.path() + u"/profile"_q));
	auto options = DesktopArchiveOptions{temp.path() + u"/archives"_q, temp.path() + u"/profile"_q,
		{0, 123}, u"0"_q, QByteArray(256, 'k'), true};
	options.loginAuthority = QByteArray(32, 'a');
	options.loginAuthKeyId = options.loginAuthKeyIds = QByteArray(16, '1');
	auto store = DesktopArchiveStore();
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	auto access = store.JournalAccess(); CHECK(access); if (!access) return;
	Value = {std::make_shared<DesktopArchiveJournal>(*access), std::make_shared<ProbeWorker>(), {}};
	CHECK(Value.journal->Open());
	auto session = ProbeSession();
	GateEntered = GateReleased = false;
	GateArmed = true;
	auto first = Event(Empty(), failed ? u"invalid id"_q : u"lifecycle"_q, 0, u"preimage"_q);
	if (!failed) first.observation.context.insert(u"desktopLifecycle"_q, 0);
	const auto start = std::chrono::steady_clock::now();
	if (failed) QueueArchiveObservation(&session, first);
	else QueueArchiveLifecycleFact(&session, first);
	const auto initial = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	CHECK(initial < 100);
	{
		auto lock = std::unique_lock(GateMutex);
		CHECK(GateCv.wait_for(lock, std::chrono::seconds(2), [] { return GateEntered.load(); }));
	}
	auto watchdog = std::thread([] {
		std::this_thread::sleep_for(std::chrono::milliseconds(600));
		{ const auto lock = std::lock_guard(GateMutex); GateReleased = true; }
		GateCv.notify_all();
	});
	auto ordinary = Event(Empty(), u"ordinary"_q, 0, u"new update"_q, ArchiveOrigin::Baseline, Key({0,123}, 45, 777));
	auto started = std::chrono::steady_clock::now();
	QueueArchiveObservation(&session, ordinary);
	const auto foreground = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
	CHECK(foreground < 100); CHECK(!GateReleased);
	for (auto i = 0; i != 10; ++i) {
		auto extra = Event(Empty(), u"queued-"_q + QString::number(i), 0, u"bounded"_q,
			ArchiveOrigin::Baseline, Key({0,123},45,1000+i));
		started = std::chrono::steady_clock::now();
		QueueArchiveObservation(&session, std::move(extra));
		CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(100));
	}
	CHECK(!GateReleased);

	{
		const auto before = FallbackLosses.load();
		const auto lock = std::lock_guard(Value.worker->_state->mutex);
		started = std::chrono::steady_clock::now();
		QueueArchiveRetentionGap(&session, u"explicit-handoff-gap"_q);
		CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(50));
		CHECK(FallbackLosses == before + 1);
		CHECK(!Value.worker->Post([](auto&) {}));
	}
	for (auto i = 0; i != 64; ++i) CHECK(Value.worker->Post([](auto&) {}) || !Value.worker->_state->tasks.empty());
	CHECK(!Value.worker->Post([](auto&) {}));
	CHECK(Value.worker->Post([](auto&) {}, true));
	watchdog.join();
	for (auto i = 0; i != 1000; ++i) {
		Value.worker->Run(store);
		if (Value.journal.use_count() == 1) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	Value.worker->Run(store);
	CHECK(Value.journal.use_count() == 1);
	const auto slice = store.ReadSlice(ordinary.key, {}, u"ordinary"_q);
	const auto version = slice ? FindArchiveVersion(slice->state, ordinary.key, u"ordinary"_q) : nullptr;
	CHECK(version && version->content.text == u"new update"_q);
	const auto gaps = store.database().Read({
		u"SELECT 1 WHERE NOT EXISTS(SELECT 1 FROM capture_gaps WHERE reason=?)"_q,
		{u"capture-durability-deadline"_q}, ArchiveSqlExpectation::OneRow });
	CHECK(gaps.ok);
	if (failed) {
		const auto attention = ReadDesktopArchiveAttention(store.directory(), store.epoch(), store.security());
		CHECK(attention.valid && attention.present && attention.gap.reason == u"outbox-not-ready"_q);
	}
	std::printf("bounded handoff error=%d initial=%lldms foreground=%lldms explicit-gap=1 ordinary-retained=1\n",
		failed, static_cast<long long>(initial), static_cast<long long>(foreground));
	Value = {};
	CHECK(store.Remove());
}
thread_local bool RenewalHoldAppend = false;
thread_local int RenewalCurrentCalls = 0;

uint64_t RenewalOrder(const DesktopArchiveJournalAccess &access, const QString &id) {
	const auto token = ArchiveSqlToken(access.epoch.account, ArchiveIndexKind::Event,
		(u"outbox/"_q + id).toUtf8(), access.security);
	const auto bytes = DesktopArchiveRead(access.root + u"/outbox/"_q + token, 1024 * 1024);
	if (!bytes) return 0;
	const auto opened = OpenArchiveEnvelope({access.epoch.account, access.epoch.storeId,
		ArchiveRecordKind::Event, token.toUtf8()}, access.security.keyId, *bytes, access.security.crypto);
	if (!opened) return 0;
	const auto object = QJsonDocument::fromJson(opened.bytes).object();
	const auto snapshot = DecodeArchiveVersionSnapshot(QJsonDocument(
		object.value(u"snapshot"_q).toObject()).toJson(QJsonDocument::Compact));
	return snapshot.snapshot ? snapshot.snapshot->version.order : 0;
}

void RenewalWriterHandoff() {
	Section = "held old writer and completed-append control retain distinct saved orders";
	for (const auto overlap : {false, true}) {
		auto temporary = QTemporaryDir();
		CHECK(DesktopArchiveDirectory(temporary.path() + u"/profile"_q));
		auto options = DesktopArchiveOptions{temporary.path() + u"/archives"_q,
			temporary.path() + u"/profile"_q, {0,123}, u"0"_q, QByteArray(256,'k'), true};
		options.loginAuthority = QByteArray(32,'a');
		options.loginAuthKeyId = options.loginAuthKeyIds = QByteArray(16,'1');
		auto store = DesktopArchiveStore(); CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
		CHECK(store.Capture(Event(Empty(), u"baseline"_q, 1, u"baseline"_q)).retained);
		auto access = store.JournalAccess(); CHECK(access && access->active());
		if (!access) continue;
		auto checked = std::promise<void>(); auto checkedFuture = checked.get_future();
		auto proceed = std::promise<void>(); auto proceedFuture = proceed.get_future().share();
		const auto current = access->current;
		access->current = [current, &checked, proceedFuture] {
			const auto valid = current();
			if (RenewalHoldAppend && ++RenewalCurrentCalls == 2) {
				checked.set_value(); static_cast<void>(proceedFuture.wait_for(std::chrono::seconds(15)));
			}
			return valid;
		};
		auto old = DesktopArchiveJournal(*access); CHECK(old.Open());
		auto event = Event(Empty(), u"old-append"_q, 2, u"old"_q, ArchiveOrigin::AuthoritativeEdit);
		old.BindCaptureGeneration(event);
		auto appended = false;
		auto writer = std::thread([&] { RenewalHoldAppend = overlap; RenewalCurrentCalls = 0; appended = old.Append(event); });
		if (overlap) CHECK(checkedFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
		else writer.join();
		const auto program = ArchiveSqlProgram{ArchiveSqlStatus::Ready, {{u"BEGIN IMMEDIATE"_q},
			{u"UPDATE archive_meta SET revision=revision WHERE singleton=1"_q, {}, ArchiveSqlExpectation::OneChange},
			{u"COMMIT"_q}}};
		store.database().FailForTest(u"prepare"_q, 3);
		CHECK(store.Execute(program) && !access->active());
		if (overlap) {
			const auto started = std::chrono::steady_clock::now();
			CHECK(!old.RetireWriter());
			CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(250));
			store.RefuseJournalRenewal(u"capture-admission-writer-retirement-held"_q);
			CHECK(store.failure() == u"capture-admission-writer-retirement-held"_q);
			CHECK(old.WriterRetired() && !old.CaptureAllowed() && !store.JournalAccess());
			old.ResumeCapture(); CHECK(!old.CaptureAllowed());
			proceed.set_value(); writer.join();
		}
		CHECK(appended && old.RetireWriter());
		const auto oldOrder = RenewalOrder(*access, u"old-append"_q); CHECK(oldOrder == 2);
		const auto renewed = store.RenewJournalAccess(); CHECK(renewed && renewed->active());
		auto fresh = renewed ? std::make_unique<DesktopArchiveJournal>(*renewed) : nullptr;
		const auto opened = fresh && fresh->Open() && fresh->Drain(store);
		CHECK(opened && store.CompleteJournalRenewal(opened));
		auto second = Event(Empty(), u"new-append"_q, 3, u"new"_q, ArchiveOrigin::AuthoritativeEdit);
		if (fresh) fresh->BindCaptureGeneration(second);
		CHECK(fresh && fresh->Append(second));
		const auto newOrder = renewed ? RenewalOrder(*renewed, u"new-append"_q) : 0;
		CHECK(newOrder == 3 && newOrder > oldOrder);
		const auto drained = fresh && fresh->Drain(store);
		const auto retryDrained = fresh && fresh->Drain(store);
		CHECK(drained && retryDrained);
		old.ResumeCapture(); CHECK(!old.CaptureAllowed() && !old.Append(second));
		CHECK(!access->active() && renewed && renewed->active());
		std::printf("RENEWAL_RACE overlap=%d old=%llu new=%llu oldAppend=%d drain=%d retryDrain=%d\n",
			int(overlap), static_cast<unsigned long long>(oldOrder), static_cast<unsigned long long>(newOrder),
			int(appended), int(drained), int(retryDrained));
		CHECK(store.Remove());
	}
}

void StartupActorCases();
void StartupReadyOrder();
void NativeStartupDiscard();
void NativeStartupInflight();
void NativeStartupUnavailable();
void NativeStartupPurgeRenewal();
void NativeStartupReplacement();
void NativeStartupPayload();
void NativeStreamAdmission();

int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv);
	RenewalWriterHandoff();
	StartupActorCases();
	StartupReadyOrder();
	NativeStartupDiscard();
	NativeStartupInflight();
	NativeStartupUnavailable();
	NativeStartupPurgeRenewal();
	NativeStartupReplacement();
	NativeStartupPayload();
	NativeStreamAdmission();
	HistoricalGapNotice();
	Case(false); Case(true);
	return Finish();
}


void StartupActorCases() {
	Section = "native startup actor retains captures without main-thread ready publication";
	for (auto mode = 0; mode != 7; ++mode) {
		auto temporary = QTemporaryDir();
		CHECK(DesktopArchiveDirectory(temporary.path() + u"/profile"_q));
		auto options = DesktopArchiveOptions{temporary.path() + u"/archives"_q,
			temporary.path() + u"/profile"_q, {0,123}, u"0"_q, QByteArray(256,'k'), true};
		options.loginAuthority = QByteArray(32,'a');
		options.loginAuthKeyId = options.loginAuthKeyIds = QByteArray(16,'1');
		const auto modeSection = "native startup actor mode " + std::to_string(mode);
		Section = modeSection.c_str();
		auto entered = std::promise<void>();
		auto enteredFuture = entered.get_future();
		auto gate = std::promise<void>();
		auto readyGate = gate.get_future().share();
		auto firstAuthority = std::atomic<bool>(true);
		options.authorityReady = [&] {
			if (firstAuthority.exchange(false)) entered.set_value();
			readyGate.wait();
			return mode != 6;
		};
		auto readyCount = std::atomic<int>(0);
		auto reported = std::atomic<int>(0);
		auto lastStatus = std::atomic<DesktopArchiveStatus>(DesktopArchiveStatus::Disabled);
		auto worker = std::make_shared<NativeStartupWorker>(std::move(options),
			[&](DesktopArchiveStatus status, QString, uint64_t) { lastStatus = status; ++reported; },
			[&](std::shared_ptr<DesktopArchiveJournal>) { ++readyCount; });
		CHECK(enteredFuture.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
		const auto total = mode == 1 ? 32 : mode == 2 ? 12 : 3;
		auto accepted = 0;
		for (auto i = 0; i != total; ++i) {
			auto event = Event(Empty(), u"startup-"_q + QString::number(i), 0,
				mode == 2 ? QString(1800000, u'x') : u"before main ready"_q,
				ArchiveOrigin::Baseline, Key({0,123},45,12000+i));
			accepted += worker->PostStartupCapture(std::move(event), mode == 5 ? 1 : 0);
		}
		if (mode == 2) {
			CHECK(accepted > 0 && accepted < total);
		} else {
			CHECK(accepted == total);
		}
		if (mode == 1) {
			CHECK(!worker->PostStartupCapture(Event(Empty(), u"overflow"_q, 0), 0));
		}
		if (mode == 3) {
			CHECK(worker->ClaimPurge());
			CHECK(!worker->PostStartupCapture(Event(Empty(), u"purge-new"_q, 0), 0));
		}
		if (mode == 4 || mode == 2) {
			CHECK(worker->Stop(false));
			CHECK(!worker->PostStartupCapture(Event(Empty(), u"stopped-new"_q, 0), 0));
			gate.set_value();
			CHECK(worker->WaitForStopped(std::chrono::seconds(5)));
			CHECK(readyCount == 0);
			worker.reset();
			continue;
		}
		auto checked = std::promise<int>();
		auto checkedFuture = checked.get_future();
		CHECK(worker->Post([&](DesktopArchiveStore &store) {
			auto retained = 0;
			for (auto i = 0; i != total; ++i) {
				const auto page = store.Versions(Key({0,123},45,12000+i));
				retained += page.versions.size() == 1;
			}
			checked.set_value(retained);
		}));
		gate.set_value();
		CHECK(checkedFuture.wait_for(std::chrono::seconds(20)) == std::future_status::ready);
		if (checkedFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
			CHECK(checkedFuture.get() == (mode == 3 || mode == 5 || mode == 6 ? 0 : accepted));
		}
		CHECK(readyCount == (mode == 6 ? 0 : 1));
		if (mode == 6) {
			CHECK(lastStatus == DesktopArchiveStatus::Unreadable);
			CHECK(worker->StartupFailed());
			CHECK(!worker->PostStartupCapture(Event(Empty(), u"terminal-new"_q, 0), 0));
		}
		CHECK(worker->Stop(false));
		CHECK(worker->WaitForStopped(std::chrono::seconds(5)));
		worker.reset();
	}
	std::printf("ARCHIVE_STARTUP_ACTOR_MODES=7\n");
}


void StartupReadyOrder() {
	Section = "startup FIFO survives main-thread journal publication";
	auto temporary = QTemporaryDir();
	CHECK(DesktopArchiveDirectory(temporary.path() + u"/profile"_q));
	auto options = DesktopArchiveOptions{temporary.path() + u"/archives"_q,
		temporary.path() + u"/profile"_q, {0,123}, u"0"_q, QByteArray(256,'k'), true};
	options.loginAuthority = QByteArray(32,'a');
	options.loginAuthKeyId = options.loginAuthKeyIds = QByteArray(16,'1');
	auto store = DesktopArchiveStore();
	CHECK(store.Open(options) == DesktopArchiveStatus::Ready);
	const auto access = store.JournalAccess();
	CHECK(access);
	if (!access) return;
	Value = {nullptr, std::make_shared<ProbeWorker>(), {}};
	Value.worker->initialJournal = std::make_shared<DesktopArchiveJournal>(*access);
	CHECK(Value.worker->initialJournal->Open());
	auto session = ProbeSession();
	for (auto i = 0; i != 3; ++i) {
		QueueArchiveObservation(&session, Event(Empty(), u"startup-first-"_q + QString::number(i),
			0, u"before ready"_q, ArchiveOrigin::Baseline, Key({0,123},45,15000+i)));
	}
	CHECK(Value.worker->NeedsStartupCapture());
	Value.journal = Value.worker->initialJournal;
	QueueArchiveObservation(&session, Event(Empty(), u"startup-edit"_q, 0,
		u"after ready"_q, ArchiveOrigin::AuthoritativeEdit, Key({0,123},45,15000)));
	CHECK(Value.worker->_state->startupCount == 4);
	Value.worker->Run(store);
	CHECK(!Value.worker->NeedsStartupCapture());
	CHECK(Value.worker->_state->startupBytes == 0);
	const auto page = store.Versions(Key({0,123},45,15000));
	CHECK(page.versions.size() == 2);
	const auto before = std::find_if(page.versions.begin(), page.versions.end(),
		[](const auto &row) { return row.version.id == u"startup-first-0"_q; });
	const auto after = std::find_if(page.versions.begin(), page.versions.end(),
		[](const auto &row) { return row.version.id == u"startup-edit"_q; });
	CHECK(before != page.versions.end() && after != page.versions.end());
	if (before != page.versions.end() && after != page.versions.end()) {
		CHECK(before->version.content.text == u"before ready"_q);
		CHECK(after->version.content.text == u"after ready"_q);
		CHECK(before->version.order < after->version.order);
	}
	Value = {};
	CHECK(store.Remove());
	std::printf("ARCHIVE_STARTUP_FIFO_HANDOFF=1\n");
}


DesktopArchiveOptions StartupOptions(const QString &path) {
	CHECK(DesktopArchiveDirectory(path + u"/profile"_q));
	auto options = DesktopArchiveOptions{path + u"/archives"_q,
		path + u"/profile"_q, {0,123}, u"0"_q, QByteArray(256,'k'), true};
	options.loginAuthority = QByteArray(32,'a');
	options.loginAuthKeyId = options.loginAuthKeyIds = QByteArray(16,'1');
	return options;
}

void NativeStartupDiscard() {
	Section = "startup worker resumes a committed discard after the account lock clears";
	auto temporary = QTemporaryDir();
	auto options = StartupOptions(temporary.path());
	auto initial = DesktopArchiveStore();
	CHECK(initial.Open(options) == DesktopArchiveStatus::Ready);
	if (!initial.Current()) {
		return;
	}
	CHECK(initial.Capture(Event(Empty(), u"old-archive"_q, 1, u"fixture"_q)).retained);
	const auto oldEpoch = initial.epoch();
	const auto oldRoot = initial.directory();
	const auto account = QFileInfo(oldRoot).absolutePath();
	const auto profile = QFileInfo(QFileInfo(account).absolutePath()).absolutePath();
	const auto detached = account + u".discard"_q;
	const auto marker = account + u".discard.json"_q;
	CHECK(initial.Close());
	const auto token = DesktopArchiveStore::PrepareDiscard(options).token;
	CHECK(token);
	if (!token) {
		return;
	}
	auto checks = 0;
	options.maintenanceAllowed = [&] { return ++checks <= 2; };
	CHECK(!DesktopArchiveStore::DiscardAccount(options, *token));
	options.maintenanceAllowed = {};
	CHECK(QFileInfo::exists(detached) && QFileInfo::exists(marker));
	auto accountLock = QLockFile(profile + u"/locks/0-123.lock"_q);
	CHECK(accountLock.tryLock(0));
	auto waiting = std::promise<void>();
	auto ready = std::promise<ArchiveStoreEpoch>();
	const auto waitingFuture = waiting.get_future();
	auto readyFuture = ready.get_future();
	auto reported = false;
	auto worker = std::make_shared<NativeStartupWorker>(options,
		[&](DesktopArchiveStatus status, QString reason, uint64_t) {
			if (status == DesktopArchiveStatus::RemovalPending
				&& reason == u"discard-account-lock-unavailable"_q
				&& !std::exchange(reported, true)) {
				waiting.set_value();
			}
		}, [&](auto journal) { ready.set_value(journal->access().epoch); });
	CHECK(waitingFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
	CHECK(QFileInfo::exists(detached) && QFileInfo::exists(marker));
	accountLock.unlock();
	if (readyFuture.wait_for(std::chrono::seconds(15)) == std::future_status::ready) {
		const auto epoch = readyFuture.get();
		CHECK(epoch.account == oldEpoch.account && epoch.storeId != oldEpoch.storeId);
		CHECK(!QFileInfo::exists(detached) && !QFileInfo::exists(marker) && !QFileInfo::exists(oldRoot));
		auto empty = std::promise<bool>();
		auto emptyFuture = empty.get_future();
		CHECK(worker->Post([&](DesktopArchiveStore &store) {
			empty.set_value(store.Versions(Key()).versions.empty());
		}));
		const auto checked = emptyFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
		CHECK(checked && emptyFuture.get());
		CHECK(worker->Stop(false));
		CHECK(worker->WaitForStopped(std::chrono::seconds(5)));
	} else {
		CHECK(false);
		CHECK(worker->Stop(false));
		CHECK(worker->WaitForStopped(std::chrono::seconds(5)));
	}
}

void NativeStartupInflight() {
	Section = "production worker retains in-flight startup charge through fsync";
	auto temporary = QTemporaryDir();
	auto options = StartupOptions(temporary.path());
	auto entered = std::promise<void>();
	auto enteredFuture = entered.get_future();
	auto gate = std::promise<void>();
	auto future = gate.get_future().share();
	options.authorityReady = [&] { entered.set_value(); future.wait(); return true; };
	auto ready = std::promise<std::shared_ptr<DesktopArchiveJournal>>();
	auto readyFuture = ready.get_future();
	NativeValue = {};
	NativeValue.worker = std::make_shared<NativeStartupWorker>(std::move(options),
		[](DesktopArchiveStatus, QString, uint64_t) {},
		[&](auto journal) { ready.set_value(std::move(journal)); });
	auto worker = NativeValue.worker;
	CHECK(enteredFuture.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
	auto published = std::promise<void>();
	auto publishedFuture = published.get_future().share();
	CHECK(worker->Post([&](DesktopArchiveStore &) {
		publishedFuture.wait();
		GateEntered = GateReleased = false;
		GateArmed = true;
	}));
	auto session = ProbeSession();
	NativeQueueArchiveObservation(&session, Event(Empty(), u"native-startup-first"_q,
		0, u"before ready"_q, ArchiveOrigin::Baseline, Key({0,123},45,17000)));
	CHECK(worker->NeedsStartupCapture());
	CHECK(NativeValue.failure.isEmpty());
	gate.set_value();
	CHECK(readyFuture.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
	if (readyFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
		NativeValue.journal = readyFuture.get();
	}
	published.set_value();
	{
		auto lock = std::unique_lock(GateMutex);
		CHECK(GateCv.wait_for(lock, std::chrono::seconds(5), [] { return GateEntered.load(); }));
	}
	CHECK(worker->NeedsStartupCapture());
	NativeQueueArchiveObservation(&session, Event(Empty(), u"native-startup-edit"_q,
		0, u"after ready"_q, ArchiveOrigin::AuthoritativeEdit, Key({0,123},45,17000)));
	auto checked = std::promise<bool>();
	auto checkedFuture = checked.get_future();
	CHECK(worker->Post([&](DesktopArchiveStore &store) {
		const auto page = store.Versions(Key({0,123},45,17000));
		const auto first = std::find_if(page.versions.begin(), page.versions.end(),
			[](const auto &v) { return v.version.id == u"native-startup-first"_q; });
		const auto edit = std::find_if(page.versions.begin(), page.versions.end(),
			[](const auto &v) { return v.version.id == u"native-startup-edit"_q; });
		const auto gaps = store.database().Read(ArchiveSqlReadCaptureGaps(32));
		checked.set_value(page.versions.size() == 2 && first != page.versions.end()
			&& edit != page.versions.end() && first->version.order < edit->version.order
			&& gaps.ok && gaps.rows.empty());
	}));
	{
		const auto lock = std::lock_guard(GateMutex);
		GateReleased = true;
	}
	GateCv.notify_all();
	CHECK(checkedFuture.wait_for(std::chrono::seconds(20)) == std::future_status::ready);
	if (checkedFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
		CHECK(checkedFuture.get());
	}
	CHECK(!worker->NeedsStartupCapture());
	CHECK(worker->Stop(false));
	CHECK(worker->WaitForStopped(std::chrono::seconds(5)));
	NativeValue = {};
	std::printf("ARCHIVE_STARTUP_NATIVE_INFLIGHT=1\n");
}

void NativeStartupUnavailable() {
	for (const auto maintenance : {false, true}) for (const auto early : {false, true}) {
		Section = maintenance ? "startup maintenance keeps durable attention" : "startup journal failure keeps durable gap";
		auto temporary = QTemporaryDir();
		auto options = StartupOptions(temporary.path());
		{
			auto setup = DesktopArchiveStore();
			CHECK(setup.Open(options) == DesktopArchiveStatus::Ready);
			if (maintenance) {
				CHECK(setup.Capture(Event(Empty(), u"before-maintenance"_q, 0)).retained);
				const auto preview = setup.PreparePurge(ArchivePurgeKind::Trashbin, {{0,123}});
				CHECK(preview.confirmation);
				if (preview.confirmation) CHECK(setup.BeginPurge(*preview.confirmation));
				CHECK(setup.MaintenancePending());
			} else {
				const auto access = setup.JournalAccess();
				CHECK(access);
				if (access) {
					auto journal = DesktopArchiveJournal(*access);
					CHECK(journal.Open());
					const auto files = QDir(access->root + u"/outbox"_q).entryList(QDir::Files);
					CHECK(files.size() == 1);
					if (files.size() == 1) {
						auto corrupt = QFile(access->root + u"/outbox/"_q + files.front());
						CHECK(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
						CHECK(corrupt.write("synthetic invalid sequence") == 26);
						corrupt.close();
					}
				}
			}
			CHECK(setup.Close());
		}
		auto entered = std::promise<void>();
		auto enteredFuture = entered.get_future();
		auto gate = std::promise<void>();
		auto future = gate.get_future().share();
		options.authorityReady = [&] { entered.set_value(); future.wait(); return true; };
		options.maintenanceAllowed = [] { return false; };
		auto readyCount = std::atomic<int>(0);
		NativeValue = {};
		NativeValue.worker = std::make_shared<NativeStartupWorker>(std::move(options),
			[](DesktopArchiveStatus, QString, uint64_t) {}, [&](auto) { ++readyCount; });
		auto worker = NativeValue.worker;
		CHECK(enteredFuture.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
		auto session = ProbeSession();
		if (early) NativeQueueArchiveObservation(&session, Event(Empty(), u"unavailable-early"_q, 0));
		auto initialized = std::promise<bool>();
		auto initializedFuture = initialized.get_future();
		auto lateScheduled = std::promise<void>();
		auto lateScheduledFuture = lateScheduled.get_future().share();
		CHECK(worker->Post([&](DesktopArchiveStore &) { initialized.set_value(true); lateScheduledFuture.wait(); }));
		gate.set_value();
		CHECK(initializedFuture.wait_for(std::chrono::seconds(15)) == std::future_status::ready);
		CHECK(worker->StartupFailed());
		CHECK(!worker->StartupHeld());
		CHECK(readyCount == 0);
		NativeQueueArchiveObservation(&session, Event(Empty(), u"unavailable-late"_q, 0));
		auto checked = std::promise<bool>();
		auto checkedFuture = checked.get_future();
		CHECK(worker->Post([&](DesktopArchiveStore &store) {
			if (maintenance) {
				const auto attention = ReadDesktopArchiveAttention(store.directory(), store.epoch(), store.security());
				checked.set_value(store.MaintenancePending() && !store.Current()
					&& attention.valid && attention.present
					&& attention.gap.reason == (early ? u"capture-startup-unavailable"_q : u"capture-write-unavailable"_q));
			} else {
				const auto gaps = store.database().Read(ArchiveSqlReadCaptureGaps(32));
				checked.set_value(gaps.ok && !gaps.rows.empty());
			}
		}));
		lateScheduled.set_value();
		CHECK(checkedFuture.wait_for(std::chrono::seconds(15)) == std::future_status::ready);
		if (checkedFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) CHECK(checkedFuture.get());
		CHECK(worker->Stop(false));
		CHECK(worker->WaitForStopped(std::chrono::seconds(5)));
		NativeValue = {};
	}
	std::printf("ARCHIVE_STARTUP_UNAVAILABLE_MODES=4\n");
}

void NativeStartupPurgeRenewal() {
	Section = "published purge retires startup generation before renewal";
	auto temporary = QTemporaryDir();
	auto options = StartupOptions(temporary.path());
	auto entered = std::promise<void>(); auto enteredFuture = entered.get_future();
	auto openGate = std::promise<void>(); auto openFuture = openGate.get_future().share();
	options.authorityReady = [&] { entered.set_value(); openFuture.wait(); return true; };
	auto first = std::promise<std::shared_ptr<DesktopArchiveJournal>>(); auto firstFuture = first.get_future();
	auto renewed = std::promise<std::shared_ptr<DesktopArchiveJournal>>(); auto renewedFuture = renewed.get_future();
	auto readyCount = std::atomic<int>(0);
	NativeValue = {};
	NativeValue.worker = std::make_shared<NativeStartupWorker>(std::move(options),
		[](DesktopArchiveStatus, QString, uint64_t) {}, [&](auto journal) {
			const auto index = readyCount.fetch_add(1);
			if (!index) first.set_value(journal);
			else if (index == 1) renewed.set_value(journal);
		});
	auto worker = NativeValue.worker;
	CHECK(enteredFuture.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
	auto prepared = std::promise<DesktopArchivePurgePreview>(); auto preparedFuture = prepared.get_future();
	auto release = std::promise<void>(); auto releaseFuture = release.get_future().share();
	CHECK(worker->Post([&](DesktopArchiveStore &store) {
		prepared.set_value(store.PreparePurge(ArchivePurgeKind::Trashbin, {{0,123}}));
		releaseFuture.wait();
	}));
	auto session = ProbeSession();
	NativeQueueArchiveObservation(&session, Event(Empty(), u"pre-purge-startup"_q, 0,
		u"old generation"_q, ArchiveOrigin::Baseline, Key({0,123},45,18000)));
	openGate.set_value();
	CHECK(firstFuture.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
	CHECK(preparedFuture.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
	NativeValue.journal = firstFuture.get();
	const auto initial = NativeValue.journal;
	const auto preview = preparedFuture.get();
	CHECK(preview.confirmation);
	auto status = std::atomic<DesktopArchiveStatus>(DesktopArchiveStatus::Disabled);
	auto permitted = std::make_shared<std::atomic<bool>>(true);
	if (preview.confirmation) NativeExecuteArchivePurge(&session, *preview.confirmation,
		permitted, [&](auto value) { status = value; });
	CHECK(!initial->CaptureAllowed());
	CHECK(!worker->PostStartupCapture(Event(Empty(), u"during-purge"_q, 0), 0));
	release.set_value();
	const auto completed = renewedFuture.wait_for(std::chrono::seconds(30)) == std::future_status::ready;
	CHECK(completed);
	if (completed) {
		NativeValue.journal = renewedFuture.get();
		CHECK(NativeValue.journal != initial);
		CHECK(initial->WriterRetired());
		CHECK(!initial->CaptureAllowed());
		CHECK(NativeValue.journal->CaptureAllowed());
		CHECK(initial->CaptureGeneration(Key({0,123},45,18000).chat)
			!= NativeValue.journal->CaptureGeneration(Key({0,123},45,18000).chat));
		CHECK(!worker->NeedsStartupCapture());
		CHECK(status == DesktopArchiveStatus::Maintenance);
		NativeQueueArchiveObservation(&session, Event(Empty(), u"post-purge-fresh"_q, 0,
			u"fresh generation"_q, ArchiveOrigin::Baseline, Key({0,123},45,18001)));
		auto checked = std::promise<bool>(); auto checkedFuture = checked.get_future();
		CHECK(worker->Post([&](DesktopArchiveStore &store) {
			checked.set_value(store.Versions(Key({0,123},45,18000)).versions.empty()
				&& store.Versions(Key({0,123},45,18001)).versions.size() == 1);
		}));
		CHECK(checkedFuture.wait_for(std::chrono::seconds(15)) == std::future_status::ready);
		if (checkedFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) CHECK(checkedFuture.get());
		CHECK(readyCount == 2);
	}
	CHECK(worker->Stop(false)); CHECK(worker->WaitForStopped(std::chrono::seconds(5)));
	NativeValue = {};
	std::printf("ARCHIVE_STARTUP_POST_READY_PURGE_RENEWAL=1\n");
}

void NativeStartupReplacement() {
	Section = "stopped startup worker cannot replay into successor";
	auto temporary = QTemporaryDir();
	auto options = StartupOptions(temporary.path());
	auto entered = std::promise<void>(); auto enteredFuture = entered.get_future();
	auto release = std::promise<void>(); auto releaseFuture = release.get_future().share();
	options.authorityReady = [&] { entered.set_value(); releaseFuture.wait(); return true; };
	auto oldReady = std::atomic<int>(0);
	auto old = std::make_shared<NativeStartupWorker>(options,
		[](DesktopArchiveStatus, QString, uint64_t) {}, [&](auto) { ++oldReady; });
	CHECK(enteredFuture.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
	CHECK(old->PostStartupCapture(Event(Empty(), u"replaced-startup"_q, 0,
		u"must stay absent"_q, ArchiveOrigin::Baseline, Key({0,123},45,19000)), 0));
	CHECK(old->NeedsStartupCapture());
	CHECK(old->Stop(false));
	CHECK(!old->NeedsStartupCapture());
	release.set_value();
	CHECK(old->WaitForStopped(std::chrono::seconds(5))); CHECK(oldReady == 0);
	auto successorEntered = std::promise<void>(); auto successorEnteredFuture = successorEntered.get_future();
	auto successorRelease = std::promise<void>(); auto successorReleaseFuture = successorRelease.get_future().share();
	options.authorityReady = [&] { successorEntered.set_value(); successorReleaseFuture.wait(); return true; };
	auto ready = std::promise<std::shared_ptr<DesktopArchiveJournal>>(); auto readyFuture = ready.get_future();
	NativeValue = {};
	NativeValue.worker = std::make_shared<NativeStartupWorker>(std::move(options),
		[](DesktopArchiveStatus, QString, uint64_t) {}, [&](auto journal) { ready.set_value(journal); });
	auto worker = NativeValue.worker;
	CHECK(successorEnteredFuture.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
	CHECK(!old->PostStartupCapture(Event(Empty(), u"late-old"_q, 0), 0));
	auto session = ProbeSession();
	NativeQueueArchiveObservation(&session, Event(Empty(), u"replacement-fresh"_q, 0,
		u"successor only"_q, ArchiveOrigin::Baseline, Key({0,123},45,19001)));
	auto checked = std::promise<bool>(); auto checkedFuture = checked.get_future();
	CHECK(worker->Post([&](DesktopArchiveStore &store) {
		checked.set_value(store.Versions(Key({0,123},45,19000)).versions.empty()
			&& store.Versions(Key({0,123},45,19001)).versions.size() == 1);
	}));
	successorRelease.set_value();
	CHECK(readyFuture.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
	NativeValue.journal = readyFuture.get();
	CHECK(checkedFuture.wait_for(std::chrono::seconds(15)) == std::future_status::ready);
	if (checkedFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) CHECK(checkedFuture.get());
	CHECK(worker->Stop(false)); CHECK(worker->WaitForStopped(std::chrono::seconds(5)));
	NativeValue = {};
	std::printf("ARCHIVE_STARTUP_REPLACEMENT=1\n");
}

void NativeStartupPayload() {
	Section = "bounded large startup payload drain measured";
	auto temporary = QTemporaryDir();
	auto options = StartupOptions(temporary.path());
	auto entered = std::promise<void>(); auto enteredFuture = entered.get_future();
	auto release = std::promise<void>(); auto releaseFuture = release.get_future().share();
	options.authorityReady = [&] { entered.set_value(); releaseFuture.wait(); return true; };
	auto readyCount = std::atomic<int>(0);
	auto worker = std::make_shared<NativeStartupWorker>(std::move(options),
		[](DesktopArchiveStatus, QString, uint64_t) {}, [&](auto) { ++readyCount; });
	CHECK(enteredFuture.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
	for (auto i = 0; i != 2; ++i) CHECK(worker->PostStartupCapture(Event(Empty(),
		u"large-startup-"_q + QString::number(i), 0, QString(1800000, u'x'),
		ArchiveOrigin::Baseline, Key({0,123},45,20000+i)), 0));
	auto checked = std::promise<int>(); auto checkedFuture = checked.get_future();
	CHECK(worker->Post([&](DesktopArchiveStore &store) {
		auto count = 0;
		for (auto i = 0; i != 2; ++i) {
			const auto rows = store.Versions(Key({0,123},45,20000+i));
			count += rows.versions.size() == 1 && rows.versions.front().version.content.text.size() == 1800000;
		}
		checked.set_value(count);
	}));
	const auto started = std::chrono::steady_clock::now();
	release.set_value();
	const auto completed = checkedFuture.wait_for(std::chrono::seconds(60)) == std::future_status::ready;
	CHECK(completed);
	if (completed) CHECK(checkedFuture.get() == 2);
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
	CHECK(readyCount == 1); CHECK(!worker->NeedsStartupCapture());
	CHECK(worker->Stop(false)); CHECK(worker->WaitForStopped(std::chrono::seconds(5)));
	std::printf("ARCHIVE_STARTUP_LARGE_PAYLOAD=2 chars=3600000 elapsed_ms=%lld\n", static_cast<long long>(elapsed));
}

void NativeStreamAdmission() {
	Section = "stream bursts preserve regular worker capacity and FIFO order";
	using Result = NativeStartupWorker::StreamAdmission;
	auto temporary = QTemporaryDir();
	auto options = StartupOptions(temporary.path());
	auto entered = std::promise<void>();
	auto enteredFuture = entered.get_future();
	auto release = std::promise<void>();
	auto releaseFuture = release.get_future().share();
	options.authorityReady = [&] {
		entered.set_value();
		releaseFuture.wait();
		return true;
	};
	const auto order = std::make_shared<std::vector<int>>();
	auto done = std::promise<void>();
	auto finished = done.get_future();
	auto worker = std::make_shared<NativeStartupWorker>(
		std::move(options),
		[](DesktopArchiveStatus, QString, uint64_t) {},
		[](auto) {});
	CHECK(enteredFuture.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
	CHECK(worker->Post([order](DesktopArchiveStore &) {
		order->push_back(-1);
	}));
	for (auto i = 0; i != 128; ++i) {
		CHECK(worker->PostStream([order, i](DesktopArchiveStore &) {
			order->push_back(i);
		}, 128 * 1024) == Result::Queued);
	}
	CHECK(worker->PostStream([](DesktopArchiveStore &) {}, 0) == Result::Full);
	CHECK(worker->Post([order, &done](DesktopArchiveStore &) {
		order->push_back(128);
		done.set_value();
	}));
	release.set_value();
	const auto drained = finished.wait_for(std::chrono::seconds(30))
		== std::future_status::ready;
	CHECK(drained);
	if (drained) {
		CHECK(order->size() == 130);
		for (auto i = 0; i != int(order->size()); ++i) {
			CHECK((*order)[i] == i - 1);
		}
		CHECK(worker->PostStream([](DesktopArchiveStore &) {}, 128 * 1024)
			== Result::Queued);
	}
	CHECK(worker->Stop(false));
	CHECK(worker->PostStream([](DesktopArchiveStore &) {}, 0) == Result::Retired);
	CHECK(worker->WaitForStopped(std::chrono::seconds(5)));
}

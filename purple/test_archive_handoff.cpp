#include "purple/purple_archive_journal.h"
#include "base/unixtime.h"
#include "purple/purple_archive_files.h"
#include "tests/archive_test_support.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtCore/QJsonDocument>
#include <future>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <set>
#include <chrono>
#include <dlfcn.h>

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
struct ProbeSession { AccountKey account{0, 123}; };
struct ProbeWorker {
	using Task = std::function<void(DesktopArchiveStore&)>;
	struct State {
		std::mutex mutex;
		std::condition_variable condition;
		std::deque<Task> tasks;
		bool cancelled = false;
	};
	std::shared_ptr<State> _state = std::make_shared<State>();
	bool Post(Task task, bool cleanup = false);
	void Run(DesktopArchiveStore &store) {
		for (;;) {
			auto task = Task();
			{
				const auto lock = std::lock_guard(_state->mutex);
				if (_state->tasks.empty()) return;
				task = std::move(_state->tasks.front());
				_state->tasks.pop_front();
			}
			task(store);
		}
	}
};
#include "archive-worker-post.inc"
struct ProbeValue {
	std::shared_ptr<DesktopArchiveJournal> journal;
	std::shared_ptr<ProbeWorker> worker;
	QString failure;
};
ProbeValue Value;
std::set<ProbeSession*> Retired;
AccountKey SessionAccountKey(not_null<ProbeSession*> session) { return session->account; }
ProbeValue &For(not_null<ProbeSession*>) { return Value; }
struct ChangePort { int count = 0; void fire(std::monostate) { ++count; } } Changes;
namespace Core {
struct Active { ProbeSession *maybeSession() { return nullptr; } };
struct Domain { bool started() { return false; } Active active() { return {}; } };
struct AppPort { Domain domain() { return {}; } };
AppPort App() { return {}; }
}
namespace tr { inline constexpr auto now = 0; QString lng_purple_archive_gap(int) { return {}; } }
namespace Ui::Toast { void Show(const QString&) {} }
void QueueArchiveRetentionGap(not_null<ProbeSession*> session, QString reason);
#include "archive-capture-queue.inc"

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
	CHECK(foreground < 100); CHECK(!GateReleased); CHECK(!Value.failure.isEmpty());
	for (auto i = 0; i != 10; ++i) {
		auto extra = Event(Empty(), u"queued-"_q + QString::number(i), 0, u"bounded"_q,
			ArchiveOrigin::Baseline, Key({0,123},45,1000+i));
		started = std::chrono::steady_clock::now();
		QueueArchiveObservation(&session, std::move(extra));
		CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(100));
	}
	CHECK(!GateReleased); CHECK(Value.failure == u"capture-write-unavailable"_q);

	{
		const auto lock = std::lock_guard(Value.worker->_state->mutex);
		started = std::chrono::steady_clock::now();
		QueueArchiveRetentionGap(&session, u"explicit-handoff-gap"_q);
		CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(50));
		CHECK(Value.failure == u"explicit-handoff-gap"_q);
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
	CHECK(Changes.count > 0);
	std::printf("bounded handoff error=%d initial=%lldms foreground=%lldms explicit-gap=1 ordinary-retained=1\n",
		failed, static_cast<long long>(initial), static_cast<long long>(foreground));
	Value = {};
	CHECK(store.Remove());
}
int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv);
	Case(false); Case(true);
	return Finish();
}

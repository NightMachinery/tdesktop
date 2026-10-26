#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;

struct State {
	std::mutex mutex;
	std::condition_variable condition;
	std::atomic<bool> streamCleanup = false;
	std::atomic<bool> cancelled = false;
	std::deque<int> tasks;
};

struct Probe {
	std::shared_ptr<State> _state = std::make_shared<State>();
#include "retirement-method.inc"
};

int main() {
	auto failures = 0;
	for (const auto forceGap : {false, true}) {
		auto probe = Probe();
		const auto state = probe._state;
		auto streamMore = false;
		auto maintenanceMore = false;
		auto garbageMore = false;
		auto checked = std::promise<void>();
		auto proceed = std::promise<void>();
		auto resumed = std::promise<void>();
		auto retiring = std::promise<void>();
		auto retired = std::promise<void>();
		auto checkedFuture = checked.get_future();
		auto proceedFuture = proceed.get_future().share();
		auto resumedFuture = resumed.get_future();
		auto retiringFuture = retiring.get_future();
		auto retiredFuture = retired.get_future();
		auto injected = false;
		auto worker = std::thread([&] {
			auto lock = std::unique_lock(state->mutex);
			state->condition.wait(lock, [&] {
				const auto ready = [&] {
#include "retirement-predicate.inc"
				}();
				if (!ready && !injected) {
					injected = true;
					checked.set_value();
					if (forceGap) proceedFuture.wait();
				}
				return ready;
			});
			resumed.set_value();
		});
		checkedFuture.wait();
		auto notifier = std::thread([&] {
			retiring.set_value();
			probe.PruneStreamsSoon();
			retired.set_value();
		});
		retiringFuture.wait();
		const auto retiredInGap = forceGap
			&& retiredFuture.wait_for(100ms) == std::future_status::ready;
		if (forceGap) proceed.set_value();
		const auto woke = resumedFuture.wait_for(500ms) == std::future_status::ready;
		notifier.join();
		const auto pending = state->streamCleanup.load();
		if (!pending || !woke || retiredInGap) ++failures;
		std::printf("retirement forceGap=%d pending=%d woke=%d retiredInGap=%d\n",
			int(forceGap), int(pending), int(woke), int(retiredInGap));
		{
			const auto lock = std::lock_guard(state->mutex);
			state->cancelled = true;
		}
		state->condition.notify_one();
		worker.join();
	}
	std::printf("2 retirement schedules, %d failures\n", failures);
	return failures != 0;
}

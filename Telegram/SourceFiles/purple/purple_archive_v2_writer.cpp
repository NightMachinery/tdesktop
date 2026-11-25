#include "purple/purple_archive_v2_writer.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace Purple::ArchiveV2 {
namespace {

constexpr auto kEvents = size_t(256);
constexpr auto kBytes = int64(4 * 1024 * 1024);

int64 Weight(const CapturedEvent &event) {
	auto bytes = int64(sizeof(CapturedEvent)) + event.storeId.size() + event.eventId.size()
		+ event.body.size() + event.preimage.size();
	for (const auto &entry : event.attachments) {
		bytes += sizeof(Attachment) + entry.source.size() + entry.description.size();
		if (bytes > kBytes) break;
	}
	return bytes;
}

}

struct Writer::State {
	struct Query {
		QByteArray generation;
		ReadOrCleanup operation;
	};
	using Job = std::variant<CapturedEvent, QByteArray, Query>;
	std::mutex mutex;
	std::condition_variable changed;
	std::deque<Job> jobs;
	std::optional<BlobInput> blob;
	bool mediaWriting = false;
	int64 bytes = 0;
	bool stopped = false;
	std::optional<QByteArray> logout;
	std::shared_ptr<Lease> lease;
	Report report;
};

Writer::Writer(StoreOptions options, Report report) : _state(std::make_shared<State>()) {
	if (!options.lease) options.lease = std::make_shared<Lease>();
	_state->lease = options.lease;
	_state->report = std::move(report);
	options.canWritePart = [weak = std::weak_ptr(_state), previous = std::move(options.canWritePart)] {
		const auto state = weak.lock();
		if (!state || (previous && !previous())) return false;
		const auto lock = std::lock_guard(state->mutex);
		return !state->stopped && state->jobs.empty();
	};
	std::thread([state = _state, options = std::move(options)]() mutable {
		run(std::move(state), std::move(options));
	}).detach();
}

Writer::~Writer() {
	stop(std::nullopt);
}

StoreEffect Writer::post(CapturedEvent event) {
	const auto size = Weight(event);
	const auto lock = std::lock_guard(_state->mutex);
	if (_state->stopped || !_state->lease->live.load()) return { u"scope_retired"_q };
	if (!_state->jobs.empty() && event.event == Observation::Baseline && !event.fillsMissing) {
		const auto last = std::get_if<CapturedEvent>(&_state->jobs.back());
		if (last && last->event == Observation::Baseline && !last->fillsMissing
			&& last->storeId == event.storeId && last->identity == event.identity && last->eventId == event.eventId) {
			return {};
		}
	}
	if (_state->jobs.size() >= kEvents || size > kBytes - _state->bytes) return { u"text_queue_full"_q };
	_state->bytes += size;
	_state->jobs.emplace_back(std::move(event));
	_state->changed.notify_one();
	return {};
}

StoreEffect Writer::postBlob(BlobInput input) {
	const auto lock = std::lock_guard(_state->mutex);
	if (_state->stopped || !_state->lease->live.load()) return { u"scope_retired"_q };
	if (_state->blob || _state->mediaWriting) return { u"store_busy"_q };
	_state->blob = std::move(input);
	_state->changed.notify_one();
	return {};
}

bool Writer::query(QByteArray expectedId, ReadOrCleanup operation) {
	const auto lock = std::lock_guard(_state->mutex);
	if (!operation || _state->stopped || !_state->lease->live.load() || _state->jobs.size() == kEvents) return false;
	_state->jobs.emplace_back(State::Query{ std::move(expectedId), std::move(operation) });
	_state->changed.notify_one();
	return true;
}

bool Writer::reset(QByteArray expectedId) {
	const auto lock = std::lock_guard(_state->mutex);
	if (_state->stopped || !_state->lease->live.load() || _state->jobs.size() == kEvents) return false;
	_state->jobs.emplace_back(std::move(expectedId));
	_state->changed.notify_one();
	return true;
}

void Writer::logout(QByteArray expectedId) {
	stop(std::move(expectedId));
}

void Writer::stop(std::optional<QByteArray> logout) {
	_state->lease->revoke();
	const auto lock = std::lock_guard(_state->mutex);
	if (_state->stopped) return;
	_state->stopped = true;
	_state->logout = std::move(logout);
	_state->changed.notify_one();
}

void Writer::run(std::shared_ptr<State> state, StoreOptions options) {
	auto store = std::make_unique<Store>(std::move(options));
	const auto report = [&](StoreEffect effect, QByteArray id) {
		if (state->report) state->report(std::move(effect), std::move(id));
	};
	auto opened = store->open();
	const auto initial = store->id();
	if (!opened) {
		store.reset();
		report(std::move(opened), initial);
		return;
	}
	if (state->lease->live.load()) {
		const auto limit = store->maintain();
		report(limit ? std::move(opened) : limit, initial);
	}
	for (;;) {
		auto lock = std::unique_lock(state->mutex);
		const auto ready = state->changed.wait_for(lock, std::chrono::minutes(1),
			[&] { return state->stopped || !state->jobs.empty() || state->blob.has_value(); });
		if (!ready) {
			lock.unlock();
			const auto result = store->maintain();
			if (!result && state->lease->live.load()) report(result, store->id());
			continue;
		}
		if (state->stopped) {
			const auto logout = std::move(state->logout);
			lock.unlock();
			if (logout) {
				auto result = store->logout(*logout);
				const auto id = store->id();
				store.reset();
				report(std::move(result), id);
			}
			return;
		}
		if (state->jobs.empty()) {
			auto blob = std::exchange(state->blob, std::nullopt);
			state->mediaWriting = true;
			lock.unlock();
			auto effect = store->writeBlob(*blob);
			{
				const auto guard = std::lock_guard(state->mutex);
				state->mediaWriting = false;
			}
			if (state->lease->live.load()) report(std::move(effect), store->id());
			continue;
		}
		auto job = std::move(state->jobs.front());
		state->jobs.pop_front();
		const auto event = std::get_if<CapturedEvent>(&job);
		if (event) state->bytes -= Weight(*event);
		lock.unlock();
		if (event) {
			auto effect = store->capture(*event);
			if (state->lease->live.load()) report(std::move(effect), store->id());
		} else if (const auto query = std::get_if<State::Query>(&job)) {
			if (store->current() && query->generation == store->id()) query->operation(*store);
		} else {
			auto effect = store->reset(std::get<QByteArray>(job));
			const auto id = store->id();
			if (!store->current()) {
				store.reset();
				report(std::move(effect), id);
				return;
			}
			report(std::move(effect), id);
		}
	}
}

}

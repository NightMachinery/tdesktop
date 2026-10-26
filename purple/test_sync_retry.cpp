#include "purple_sync_retry.h"

#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace Purple;
using mtpRequestId = int64_t;
using QString = std::string;
template <typename T> using Fn = std::function<T>;
static auto Mono = int64_t(0);
static auto Unix = int64_t(1000000);
namespace crl { int64_t now() { return Mono; } }
namespace base {
template <typename T> T take(T &v) { return std::exchange(v, {}); }
namespace unixtime { int64_t now() { return Unix; } }
}
std::string operator""_q(const char16_t *text, size_t size) {
	return std::string(text, text + size);
}
namespace MTP {
struct Text : std::string {
	using std::string::string;
	Text toUtf8() const { return *this; }
	const char *constData() const { return data(); }
};
struct Error {
	int value = 0;
	Text name;
	int code() const { return value; }
	Text type() const { return name; }
	static Error Local(std::string name, std::string) {
		return { -1, Text(name.c_str()) };
	}
};
}
struct Timer {
	int64_t due = 0;
	void cancel() { due = 0; }
};
struct Api {
	struct Cancellation {
		Api *api;
		void cancel() { ++api->cancelled; }
	};
	int cancelled = 0;
	Cancellation request(mtpRequestId) { return { this }; }
};
class SyncRequestSender {
public:
	struct Pending {
		explicit Pending(int64_t now) : policy(now) {
		}
		SyncRetryState policy;
		Timer timer;
		Fn<void()> dispatch;
		Fn<void()> expired;
		Fn<void(Fn<void(bool)>)> resume;
		Fn<void(const MTP::Error &)> fail;
		std::optional<SyncRequestTransient> transient;
		mtpRequestId id = 1;
		mtpRequestId wire = 0;
		int64_t retryAtMs = 0;
		uint64_t generation = 0;
		bool active = true;
	};
	Fn<bool()> _current;
	Fn<void(mtpRequestId, SyncRequestTransient)> _deferred;
	Api _api;
	std::shared_ptr<int> lifetime = std::make_shared<int>(0);
	std::map<mtpRequestId, std::shared_ptr<Pending>> _pending;
	int publishes = 0;
	void publish() { ++publishes; }
	void arm(const std::shared_ptr<Pending> &p, int64_t ms) {
		p->timer.due = Mono + std::max(int64_t(1), ms);
	}
	void dispatch(const std::shared_ptr<Pending> &pending);
	void failed(const std::shared_ptr<Pending> &pending,
		const MTP::Error &error, bool timeout = false);
	void tick(const std::shared_ptr<Pending> &pending);
	void resume(const std::shared_ptr<Pending> &pending);
	void cancel(mtpRequestId id);

};

namespace base {
template <typename T> struct Weak {
	T *pointer;
	std::weak_ptr<int> lifetime;
	explicit operator bool() const { return !lifetime.expired(); }
	T *get() const { return lifetime.expired() ? nullptr : pointer; }
};
template <typename T> Weak<T> make_weak(T *pointer) {
	return { pointer, pointer->lifetime };
}
}

#include "sync-retry-production.inc"

static auto Checks = 0;
static auto Failures = 0;
void Check(bool value, const char *name) {
	++Checks;
	if (!value) {
		++Failures;
		std::cerr << "FAIL " << name << '\n';
	}
}
struct Run {
	SyncRequestSender sender;
	std::shared_ptr<SyncRequestSender::Pending> pending;
	bool current = true;
	int sends = 0;
	int finals = 0;
	int permanentCode = 0;
	std::string permanentType;
	std::vector<std::string> payloads;
	Run() {
		Mono = 0;
		Unix = 1000000;
		pending = std::make_shared<SyncRequestSender::Pending>(Mono);
		sender._pending.emplace(1, pending);
		sender._current = [this] { return current; };
		pending->dispatch = [this, payload = std::string("same-peer/body/random-id/part") ] {
			payloads.push_back(payload);
			pending->wire = ++sends;
		};
		pending->fail = [this](const MTP::Error &error) {
			++finals;
			permanentCode = error.code();
			permanentType = error.type();
		};
		sender.dispatch(pending);
	}
	void Advance(int64_t ms) {
		Mono += ms;
		Unix = 1000000 + Mono / 1000;
		if (pending->timer.due && Mono >= pending->timer.due) {
			pending->timer.due = 0;
			sender.tick(pending);
		}
	}
};

int main() {
	for (const auto type : { "FLOOD_WAIT_8", "FLOOD_PREMIUM_WAIT_8" }) {
		auto run = Run();
		run.sender.failed(run.pending, {420, MTP::Text(type)});
		Check(run.finals == 0 && run.sends == 1, "short flood is not final");
		Check(run.pending->transient && run.pending->transient->shortRetry
			&& run.pending->transient->retryAt == 1000009, "short retry at X+1");
		Check(run.pending->policy.deadline() == 39000, "deadline extended only by wait");
		run.Advance(8999);
		Check(run.sends == 1, "never early retry");
		run.Advance(1);
		Check(run.sends == 2 && !run.pending->transient, "same request retried");
		Check(run.payloads[0] == run.payloads[1], "immutable payload random-id peer part");
		run.sender.failed(run.pending, {420, MTP::Text(type)});
		Check(run.pending->transient && !run.pending->transient->shortRetry,
			"at most one inline short retry across deferred runs");
		Check(run.pending->timer.due == 17000, "one deferred timer");
		run.Advance(8000);
		Check(run.sends == 3 && run.finals == 0, "caller reschedules deferred transient");
	}
	{
		auto run = Run();
		run.sender.failed(run.pending, {420, MTP::Text("FLOOD_WAIT_90")});
		Check(run.pending->transient && !run.pending->transient->shortRetry
			&& run.pending->transient->retryAt == 1000090, "long flood distinct RetryAt");
		run.Advance(89999);
		Check(run.sends == 1 && run.finals == 0, "long wait no early/final unknown");
		run.Advance(1);
		Check(run.sends == 2, "long wait recovers without user restart");
	}
	for (const auto &error : { MTP::Error{500, MTP::Text("INTERNAL")},
			MTP::Error{503, MTP::Text("OTHER")},
			MTP::Error{-1, MTP::Text("TRANSPORT")},
			MTP::Error{400, MTP::Text("INTERNAL_SERVER_ERROR")} }) {
		auto run = Run();
		for (auto i = 0; i != 9; ++i) {
			run.sender.failed(run.pending, error);
			Check(run.pending->transient && run.pending->transient->delayMs >= 5000
				&& run.pending->transient->delayMs <= 60000, "bounded exponential backoff");
			Check(run.finals == 0, "transient cannot become null/ineligible/gone");
			run.Advance(run.pending->transient->delayMs);
			Check(run.sends == i + 2, "one dispatch per deferred deadline");
		}
	}
	for (const auto name : { "CHANNEL_PRIVATE", "USER_ID_INVALID", "PEER_ID_INVALID",
			"MESSAGE_EDIT_TIME_EXPIRED", "MESSAGE_NOT_MODIFIED", "MESSAGE_DELETE_FORBIDDEN" }) {
		auto run = Run();
		run.sender.failed(run.pending, {400, MTP::Text(name)});
		Check(run.finals == 1 && run.permanentType == name && run.permanentCode == 400,
			"permanent mapping unchanged");
		run.Advance(100000);
		Check(run.sends == 1 && run.finals == 1, "permanent never retries");
	}
	{
		auto run = Run();
		run.Advance(29000);
		run.sender.failed(run.pending, {420, MTP::Text("FLOOD_WAIT_8")});
		Check(run.pending->transient && !run.pending->transient->shortRetry,
			"short flood outside remaining deadline is deferred");
		Check(run.pending->policy.deadline() == 67000, "deferred attempt gets fresh deadline");
	}
	{
		auto run = Run();
		run.sender.failed(run.pending, {420, MTP::Text("FLOOD_WAIT_30")});
		Check(run.pending->transient && run.pending->transient->shortRetry
			&& run.pending->transient->delayMs == 31000, "inclusive 30s boundary");
	}
	{
		auto run = Run();
		run.Advance(30000);
		Check(run.pending->transient && run.pending->transient->kind == SyncTransientKind::Timeout
			&& run.finals == 0 && run.sender._api.cancelled == 1, "own timeout cancels wire and defers");
		run.Advance(5000);
		Check(run.sends == 2, "timeout resumes same stream");
	}
	for (const auto cancel : { false, true }) {
		auto run = Run();
		run.sender.failed(run.pending, {420, MTP::Text("FLOOD_WAIT_8")});
		if (cancel) { run.sender.cancel(1); } else { run.current = false; }
		run.Advance(9000);
		run.sender.tick(run.pending);
		Check(run.sends == 1, "cancel/logout/binding loss prevents retry");
		Check(!run.pending->active && !run.pending->timer.due,
			"retired callback cannot retain timer");
	}
	for (const auto suffix : { "", "garbage", "2147483648", "99999999999999999999999999" }) {
		auto run = Run();
		auto type = MTP::Text("FLOOD_WAIT_");
		type += suffix;
		run.sender.failed(run.pending, {420, type});
		Check(run.pending->transient && run.pending->transient->retryAt > Unix
			&& run.pending->transient->delayMs > 0, "malformed/oversized flood stays transient bounded integer");
	}
	{
		auto run = Run();
		run.sender.failed(run.pending, {420, MTP::Text("FLOOD_WAIT_2147483648")});
		Check(run.pending->transient && run.pending->transient->retryAt == Unix + 2147483648LL,
			"wide flood duration never truncates to int32");
	}

	for (const auto stage : { 0, 1, 2 }) {
		auto run = Run();
		run.sender._current = [&] {
			run.sender.cancel(1);
			return false;
		};
		if (stage == 0) { run.sender.dispatch(run.pending); }
		if (stage == 1) { run.sender.failed(run.pending, {500, MTP::Text("INTERNAL")}); }
		if (stage == 2) { run.sender.tick(run.pending); }
		Check(run.sends == 1 && run.finals == 0 && !run.pending->active,
			"reentrant origin predicate cancellation wins");
	}
	{
		auto state = SyncRetryState(0);
		const auto result = state.failed(420, "FLOOD_WAIT_0", 30000, Unix);
		Check(result && !result->shortRetry, "expired deadline never inline retries");
	}
	std::cout << Checks << " checks, " << Failures << " failures\n";
	return Failures ? 1 : 0;
}

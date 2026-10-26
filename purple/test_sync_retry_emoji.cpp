#define main RetainedRetryIntegrationMain
#include "test_sync_retry_integration.cpp"
#undef main
#include "emoji-queue-header.inc"
#include "emoji-queue.inc"
#include <limits>

struct EmojiExporterProbe {
	mtpRequestId _request = 0;
	EmojiExportQueue _queue;
	struct Clock { int64_t offset = 12345; int64_t elapsed() const { return Mono + offset; } } _clock;
	struct Entry { uint64_t id = 0; };
	std::vector<Entry> _entries{{42}, {43}};
	size_t _index = 0;
	struct Epoch { bool accepts(int token) const { return token == 7; } } _epoch;
	bool current = true;
	int advances = 0;
	int reconciles = 0;
	EmojiExporterProbe() { _queue.begin({42, 43}, {}); }
	bool admitted() const { return current; }
	void reconcile() { ++reconciles; }
	void next() { ++advances; _queue.next(); }
	void deferred(mtpRequestId request, const SyncRequestTransient &transient);
	void permanent(int token, uint64_t id) {
#include "emoji-permanent-fail.inc"
	}
};
#include "emoji-deferred.inc"

#include "emoji-publication-constant.inc"

struct EmojiPublicationProbe {
	EmojiExportQueue _queue;
	struct Clock { int64_t value = 0; int64_t elapsed() const { return value; } } _clock;
	struct Timer {
		int delay = 0;
		int starts = 0;
		void start(int value) { delay = value; ++starts; }
	} _debounce;
	struct Epoch { bool accepts(quint64 token) const { return token == 7; } } _epoch;
	quint64 _workerGeneration = 7;
	bool _rendering = true;
	bool _running = true;
	bool _pending = false;
	bool admitted() const { return true; }
	void reconcile() { Check(false, "publication authority remains current"); }
	void outputFailed() { Check(false, "publication succeeds"); }
	void complete(quint64 generation = 7, bool ok = true, quint64 token = 7)
#include "emoji-publication-completion.inc"
};

struct EmojiRequestRun {
	SyncRequestSender sender;
	EmojiExporterProbe exporter;
	int done = 0;
	int deferred = 0;
	EmojiRequestRun() {
		Mono = 0; Unix = 1000000;
		sender._current = [this] { return exporter.current; };
		sender._deferred = [this](mtpRequestId id, SyncRequestTransient transient) {
			++deferred;
			Check(!sender._pending.contains(id), "handoff retires controller before owner callback");
			exporter.deferred(id, transient);
		};
		exporter._request = sender.send(FakeRequest{"emoji-document-42", 42},
			[this](const int &) { ++done; },
			[this](const MTP::Error &) { exporter.permanent(7, 42); }, 0, {});
	}
	Api::Event wire() const { return sender._api.events.rbegin()->second; }
	void advance(int64_t ms) {
		Mono += ms; Unix = 1000000 + Mono / 1000;
		const auto pending = sender._pending;
		for (const auto &[id, value] : pending) {
			if (value->timer.due && value->timer.due <= Mono) {
				value->timer.due = 0;
				sender.tick(value);
			}
		}
	}
};

int main() {
	for (const auto type : {"FLOOD_WAIT_8", "FLOOD_PREMIUM_WAIT_8"}) {
		auto r = EmojiRequestRun(); const auto first = r.wire();
		first.fail({420, MTP::Text(type)});
		Check(r.deferred == 0 && r.sender._api.events.size() == 1, "short flood remains one logical pending request");
		Check(!r.exporter._queue.retryAt(r.exporter._clock.elapsed()) && r.exporter.advances == 0, "short wait is not a document failure or queue cooldown");
		r.advance(8999); Check(r.sender._api.events.size() == 1, "short flood never retries early");
		r.advance(1); Check(r.sender._api.events.size() == 2, "one short flood retries at X+1");
		Check(r.wire().request.payload == first.request.payload && r.wire().request.random == first.request.random, "metadata identity retained");
		r.wire().fail({420, MTP::Text(type)});
		Check(r.deferred == 1 && r.sender._pending.empty() && !r.exporter._request, "second flood hands off instead of another inline retry");
		Check(r.exporter._queue.retryAt(r.exporter._clock.elapsed()) == 29345, "retryAt is projected from Unix to exporter elapsed clock");
		Check(!r.exporter._queue.cooling(42, r.exporter._clock.elapsed()), "transient never marks document failed");
		Check(r.exporter._queue.resolve(43, 29344) == EmojiResolveAdmission::Cooldown, "other metadata cannot bypass flood wait");
		Check(r.exporter._queue.resolve(43, 29345) == EmojiResolveAdmission::Ready, "metadata resumes at exact projected deadline");
		Check(!r.exporter._queue.retryAt(29345), "expired transient does not cause a refresh loop");
		first.done(1); Check(r.done == 0, "retired old callback cannot publish a document");
	}
	for (const auto &error : {MTP::Error{420, MTP::Text("FLOOD_WAIT_90")},
		MTP::Error{420, MTP::Text("FLOOD_PREMIUM_WAIT_90")},
		MTP::Error{500, MTP::Text("INTERNAL")}, MTP::Error{503, MTP::Text("SERVER")},
		MTP::Error{-1, MTP::Text("TRANSPORT")}}) {
		auto r = EmojiRequestRun(); const auto old = r.wire(); old.fail(error);
		const auto seconds = error.code() == 420 ? int64_t(90) : int64_t(5);
		const auto at = int64_t(12345) + seconds * 1000;
		Check(r.deferred == 1 && r.sender._pending.empty() && !r.exporter._request, "long transient handed off once");
		Check(r.exporter._queue.retryAt(r.exporter._clock.elapsed()) == at && r.exporter.advances == 1, "transient feeds existing cooldown scheduling");
		Check(!r.exporter._queue.cooling(42, 12345), "server failure is not a document failure");
		r.exporter._queue.begin({42, 43}, {});
		Check(r.exporter._queue.resolve(42, at - 1) == EmojiResolveAdmission::Cooldown, "refresh preserves metadata cooldown");
		Check(r.exporter._queue.resolve(42, at) == EmojiResolveAdmission::Ready, "deadline automatically permits retry");
		r.advance(seconds * 1000 + 30000);
		Check(r.sender._api.events.size() == 1 && r.done == 0, "handoff has no second controller retry timer");
		old.done(1); old.fail(error); Check(r.deferred == 1 && r.done == 0, "late callbacks cannot duplicate handoff");
	}
	{
		auto r = EmojiRequestRun(); r.advance(30000);
		Check(r.deferred == 1 && r.sender._pending.empty(), "own timeout is transient, not document failure");
		Check(r.exporter._queue.retryAt(r.exporter._clock.elapsed()) == 47345, "timeout uses bounded retryAt after its response deadline");
		Check(!r.exporter._queue.cooling(42, 42345), "timeout preserves document eligibility");
	}
	{
		auto r = EmojiRequestRun(); r.wire().fail({400, MTP::Text("DOCUMENT_INVALID")});
		Check(r.deferred == 0 && !r.exporter._request && r.exporter.advances == 1, "permanent mapping unchanged");
		Check(r.exporter._queue.cooling(42, 12345) && !r.exporter._queue.cooling(43, 12345), "permanent failure keeps document-only minute cooldown");
		Check(r.exporter._queue.retryAt(12345) == 72345, "permanent one-minute deadline unchanged");
	}
	{
		auto r = EmojiRequestRun(); r.exporter.current = false; r.wire().fail({420, MTP::Text("FLOOD_WAIT_90")});
		Check(r.deferred == 0 && r.exporter.reconciles == 1 && !r.exporter._queue.retryAt(), "lost authority neither defers nor marks failed");
	}
	{
		auto r = EmojiRequestRun(); const auto old = r.wire(); r.sender.cancelAll();
		old.fail({420, MTP::Text("FLOOD_WAIT_90")}); old.done(1);
		Check(r.deferred == 0 && r.done == 0 && !r.exporter._queue.retryAt(), "logout/discard cancellation fences callbacks");
	}
	{
		auto r = EmojiRequestRun(); const auto id = r.exporter._request;
		r.exporter.deferred(id + 1, {SyncTransientKind::Flood, Unix + 90, 90000, false});
		Check(r.exporter._request == id && r.exporter.advances == 0, "superseded request cannot cool or advance current document");
	}
	{
		auto queue = EmojiExportQueue(); queue.begin({42}, {});
		queue.transient(1000090, 1000000, 12345); queue.transient(1000005, 1000000, 12345);
		Check(queue.retryAt(12345) == 102345, "merged transient never shortens active deadline");
		queue.succeeded(42); Check(queue.retryAt(12345) == 102345, "cached artwork success does not clear metadata hold");
		queue.clear(); Check(!queue.retryAt(), "revocation clears transient cooldown");
		queue.transient(1000090, 1000001, 12345, 90000);
		Check(queue.retryAt(12345) == 102345, "wall second rollover never shortens the monotonic wait");
		queue.clear();
		queue.begin({42}, {}); queue.transient(std::numeric_limits<int64_t>::max(), 0, 12345);
		Check(queue.retryAt(12345) == std::numeric_limits<int64_t>::max(), "extreme retryAt saturates without overflow");
	}
	{
		auto r = EmojiPublicationProbe();
		r._queue.begin({42, 43}, {});
		r._queue.failed(42, 0);
		r._queue.transient(1003600, 1000000, 0, 3600000);
		r.complete();
		Check(r._debounce.delay == 60000, "future permanent cooldown stays ahead of global hold");
		r._clock.value = 60000;
		r._pending = false;
		r._running = true;
		r._queue.begin({42, 43}, {});
		Check(r._queue.resolve(42, 60000) == EmojiResolveAdmission::Cooldown, "expired document cannot resolve during global hold");
		Check(r._queue.retryAt(60000) == 3600000, "overlap next deadline is the active global hold, never expired document time");
		r.complete();
		Check(r._debounce.delay == 60000, "literal publication scheduler keeps its bounded minute tick without a one-second loop");
		for (auto i = 0; i != 10; ++i) {
			r._clock.value += r._debounce.delay;
			r._pending = false;
			r._running = true;
			r._queue.begin({42, 43}, {});
			r.complete();
			Check(r._debounce.delay == 60000, "repeated publication never chooses an expired per-document timestamp during hold");
		}
		Check(r._clock.value == 660000, "ten overlap refreshes consume ten minutes instead of ten seconds");
		r._queue.failed(43, r._clock.value - 59000);
		Check(r._queue.retryAt(r._clock.value) == r._clock.value + 1000, "earlier future cached or media work is retained during global hold");
		r._pending = false; r._running = true; r.complete();
		Check(r._debounce.delay == 1000, "literal scheduler admits genuinely future media cooldown");
	}
	{
		auto r = EmojiPublicationProbe(); r._queue.begin({42}, {});
		r._queue.transient(1003600, 1000000, 0, 3600000);
		Check(r._queue.retryAt(0) == 3600000, "isolated global hold deadline is unchanged");
		r.complete(); Check(r._debounce.delay == 60000, "isolated global hold retains bounded publication tick");
	}
	{
		auto r = EmojiPublicationProbe(); r._queue.begin({42}, {}); r._queue.failed(42, 0);
		r.complete(); Check(r._debounce.delay == 60000, "permanent-only publication still waits one minute");
		r._clock.value = 60000; r._pending = false; r._running = true;
		Check(r._queue.resolve(42, 60000) == EmojiResolveAdmission::Ready, "permanent-only metadata is admitted after cooldown");
		r._queue.succeeded(42); r.complete();
		Check(r._debounce.starts == 1 && !r._pending, "successful permanent-only retry leaves no publication timer");
	}
	std::cout << "EMOJI TRANSIENT " << Checks << " checks, " << Failures << " failures\n";
	return Failures ? 1 : 0;
}

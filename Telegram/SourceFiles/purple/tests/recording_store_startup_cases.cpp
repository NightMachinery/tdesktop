#include "rpl/event_stream.h"
#include "rpl/lifetime.h"
#include "rpl/rpl.h"

#include <iostream>
#include <memory>
#include <mutex>
#include <vector>

namespace Main {
struct Session {};
struct Account {
	Session session;
	bool ready = true;
	rpl::event_stream<Session*> changes;
	rpl::producer<Session*> sessionChanges() const { return changes.events(); }
	Session *maybeSession() { return ready ? &session : nullptr; }
};
} // namespace Main

namespace {
struct Domain {
	struct Entry { std::unique_ptr<Main::Account> account; };
	std::vector<Entry> entries;
	rpl::event_stream<> accountChanges;
	rpl::event_stream<Main::Session*> activeChanges;
	const auto &accounts() const { return entries; }
	rpl::producer<> accountsChanges() const { return accountChanges.events(); }
	rpl::producer<Main::Session*> activeSessionChanges() const { return activeChanges.events(); }
};
struct Application {
	Domain owned;
	Domain &domain() { return owned; }
};
struct Store {
	std::mutex mutex;
	rpl::lifetime lifetime;
	bool loaded = false;
	std::shared_ptr<int> key;
};
int loads = 0, watches = 0, captureWatches = 0, roundStarts = 0;
Application application;
Store store;
Store &TheStore() { return store; }
void Load(Main::Account *) { ++loads; store.loaded = true; store.key = std::make_shared<int>(1); }
void Watch(Main::Session *) { ++watches; }
void WatchCapture() { ++captureWatches; }
void StartRoundCopies() { ++roundStarts; }
} // namespace
namespace Core { Application &App() { return application; } }

namespace Purple {
#include "recording-store-startup.inc"
} // namespace Purple

int main() {
	auto checks = 0, failures = 0;
	const auto expect = [&](bool value, const char *name) {
		++checks;
		if (!value) { ++failures; std::cerr << name << '\n'; }
	};
	auto &domain = application.domain();
	Purple::StartRecordingStore();
	expect(loads == 0 && !Purple::RecordingStoreAvailable(), "startup before stored accounts is unavailable");
	domain.entries.push_back({ std::make_unique<Main::Account>() });
	const auto account = domain.entries.back().account.get();
	expect(loads == 0, "restoring accounts does not emit accountsChanges");
	domain.activeChanges.fire_copy(&account->session);
	expect(loads == 1 && Purple::RecordingStoreAvailable(), "activating the restored account loads recovery");
	expect(watches == 1 && captureWatches == 1, "restored session and native capture are watched");
	Purple::StartRecordingStore();
	expect(roundStarts == 1, "starting twice adds no second service");
	domain.activeChanges.fire_copy(&account->session);
	expect(loads == 2, "a later active-session event refreshes once");
	account->changes.fire_copy(&account->session);
	expect(loads == 3 && watches == 3, "refresh replaces account subscriptions");
	store.key.reset();
	expect(!Purple::RecordingStoreAvailable(), "missing local key cannot appear as an empty ready bin");
	store.key = std::make_shared<int>(1); store.loaded = false;
	expect(!Purple::RecordingStoreAvailable(), "unreadable store stays unavailable with a key");
	std::cout << (checks - failures) << '/' << checks << " recording startup checks passed\n";
	return failures ? 1 : 0;
}

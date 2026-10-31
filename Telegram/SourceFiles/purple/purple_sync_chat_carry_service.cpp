#include "purple/purple_sync_chat_carry_service.h"

#include "base/weak_ptr.h"
#include "core/application.h"
#include "crl/crl_on_main.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "purple/purple_account_files.h"
#include "purple/purple_config.h"
#include "purple/purple_sync_chat_carry_gate.h"
#include "purple/purple_sync_chat_carry_owner.h"
#include "purple/purple_sync_chat_native.h"
#include "purple/purple_sync_chat_peer_request.h"
#include "purple/purple_sync_chat_service.h"
#include "purple/purple_sync_chat_setup_service.h"

#include <QtCore/QDir>
#include <QtGui/QGuiApplication>
#include <rpl/event_stream.h>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace Purple {
namespace {

// The carry store binds to the thread that opened it, so one account's store tasks all run
// on one dedicated thread, as the archive worker does. A queue whose thread can change
// between tasks would hold every carry.
class StoreThread final {
public:
	StoreThread() : _state(std::make_shared<State>()) {
		std::thread([state = _state] { Run(state); }).detach();
	}
	~StoreThread() {
		const auto lock = std::lock_guard(_state->mutex);
		_state->stop = true;
		_state->condition.notify_one();
	}
	// Tasks posted after stop still run until the queue drains; later ones are dropped.
	[[nodiscard]] Fn<void(Fn<void()>)> poster() const {
		return [state = _state](Fn<void()> task) {
			auto lock = std::unique_lock(state->mutex);
			if (state->exited) { return; }
			state->tasks.push_back(std::move(task));
			state->condition.notify_one();
		};
	}

private:
	struct State {
		std::mutex mutex;
		std::condition_variable condition;
		std::deque<Fn<void()>> tasks;
		bool stop = false;
		bool exited = false;
	};
	static void Run(std::shared_ptr<State> state) {
		auto lock = std::unique_lock(state->mutex);
		while (true) {
			state->condition.wait(lock, [&] { return state->stop || !state->tasks.empty(); });
			if (state->tasks.empty()) { state->exited = true; return; }
			auto task = std::move(state->tasks.front());
			state->tasks.pop_front();
			lock.unlock();
			try { task(); } catch (...) {}
			task = nullptr;
			lock.lock();
		}
	}
	std::shared_ptr<State> _state;
};

std::optional<QString> CarryDirectory(const DesktopSyncChatDiscoveryScope &scope) {
	const auto files = scope.root.isEmpty() ? std::nullopt
		: ResolveAccountFiles(scope.origin.account, scope.owner, AccountFileLayout::Desktop);
	return files ? std::optional(QDir(scope.root).filePath(files->syncDirectory)) : std::nullopt;
}

// The native dispatcher and mark port. Renewal entries pass through unchanged: both sides call
// them on the GUI thread and answer there.
DesktopSyncCarryTransport NativeTransport(not_null<Main::Session*> session, Fn<bool()> current,
		const DesktopSyncChatSetupSnapshot &opening, DesktopSyncChatCarryRenewal renewal) {
	return MakeDesktopSyncCarryTransport(session, std::move(current), DesktopSyncNativeContext{
		opening,
		DesktopSyncNativeRenewal{
			std::move(renewal.effect),
			std::move(renewal.beginCleanup),
			std::move(renewal.cleanup),
		},
	});
}

DesktopSyncChatMarkPort NativeMarkPort(not_null<Main::Session*> session, Fn<bool()> current,
		const DesktopSyncChatSetupSnapshot &opening) {
	return MakeDesktopSyncMarkNativePort(session, std::move(current), opening);
}

bool ResumeActivated() {
	return DesktopSyncChatCarrySwitchActivated(DesktopSyncChatCarrySwitch::Move)
		|| DesktopSyncChatCarrySwitchActivated(DesktopSyncChatCarrySwitch::Recovery)
		|| DesktopSyncChatCarrySwitchActivated(DesktopSyncChatCarrySwitch::FollowFill);
}

}

class DesktopSyncChatCarryService::Impl final : public base::has_weak_ptr {
public:
	explicit Impl(not_null<Main::Session*> session);
	~Impl();

	DesktopSyncChatCarryOwner &owner() { return *_owner; }
	rpl::producer<DesktopSyncChatCarryState> changes() const { return _changes.events(); }
	void startup(Fn<void()> then);

private:
	[[nodiscard]] bool current() const;
	[[nodiscard]] DesktopSyncChatCarryHost host();
	void inspect(PeerToken chat, Fn<void(std::optional<DesktopSyncChatPeer>)> done);
	void provision(DesktopSyncChatMoveDraft draft, bool resume, DesktopSyncChatProvisionDone done);
	void revoke(QString reason, bool resumeLater = false);
	void changed(const DesktopSyncChatCarryState &state);
	void settled(DesktopSyncChatCarryKind kind);
	void resumeWhenActive();

	base::weak_ptr<Main::Session> _session;
	std::optional<DesktopSyncChatDiscoveryScope> _scope;
	StoreThread _thread;
	std::unique_ptr<DesktopSyncChatCarryOwner> _owner;
	std::map<uint64_t, std::unique_ptr<DesktopSyncChatPeerRequest>> _requests;
	std::unique_ptr<DesktopSyncChatSetupService> _provision;
	rpl::event_stream<DesktopSyncChatCarryState> _changes;
	Fn<void()> _startup;
	uint64_t _requestId = 0;
	uint64_t _provisionGeneration = 0;
	bool _resumePending = false;
	rpl::lifetime _lifetime;

};

DesktopSyncChatCarryService::Impl::Impl(not_null<Main::Session*> session)
: _session(session)
, _scope(CaptureSyncChatCarryScope(session)) {
	_owner = std::make_unique<DesktopSyncChatCarryOwner>(host());
	const auto weak = base::make_weak(this);
	const auto changed = [weak] {
		const auto self = weak.get();
		const auto session = self ? self->_session.get() : nullptr;
		if (!session) { return; }
		const auto scope = CaptureSyncChatCarryScope(session);
		if (scope != self->_scope) {
			self->revoke(u"account-context-changed"_q);
			self->_scope = scope;
		}
	};
	SettingsChanges() | rpl::on_next(changed, _lifetime);
	StateChanges() | rpl::on_next(changed, _lifetime);
	Core::App().appDeactivatedValue() | rpl::on_next([weak](bool inactive) {
		const auto self = weak.get();
		if (!self) { return; }
		if (inactive) { self->revoke(u"app-inactive"_q, true); } else { self->resumeWhenActive(); }
	}, _lifetime);
	Core::App().passcodeLockChanges() | rpl::on_next([weak](bool locked) {
		const auto self = weak.get();
		if (!self) { return; }
		if (locked) { self->revoke(u"app-locked"_q, true); } else { self->resumeWhenActive(); }
	}, _lifetime);
	session->account().sessionChanges() | rpl::on_next([weak, session](Main::Session *value) {
		if (value != session) { if (const auto self = weak.get()) { self->revoke(u"account-switched"_q); } }
	}, _lifetime);
}

DesktopSyncChatCarryService::Impl::~Impl() {
	_lifetime.destroy();
	_provision.reset();
	_requests.clear();
	_owner.reset();
}

bool DesktopSyncChatCarryService::Impl::current() const {
	const auto session = _session.get();
	return session && _scope && !session->account().loggingOut()
		&& session->account().maybeSession() == session
		&& QGuiApplication::applicationState() == Qt::ApplicationActive
		&& !Core::App().passcodeLocked()
		&& CachedSyncChatSetupContextCurrent(session, *_scope);
}

DesktopSyncChatCarryHost DesktopSyncChatCarryService::Impl::host() {
	const auto weak = base::make_weak(this);
	auto host = DesktopSyncChatCarryHost();
	host.activated = [](DesktopSyncChatCarrySwitch value) { return DesktopSyncChatCarrySwitchActivated(value); };
	host.current = [weak] { const auto self = weak.get(); return self && self->current(); };
	host.scope = [weak]() -> std::optional<DesktopSyncChatDiscoveryScope> {
		const auto self = weak.get();
		return self && self->current() ? CaptureSyncChatCarryScope(self->_session.get()) : std::nullopt;
	};
	host.worker = _thread.poster();
	host.main = [](Fn<void()> task) { crl::on_main(std::move(task)); };
	host.directory = [](const DesktopSyncChatDiscoveryScope &scope) { return CarryDirectory(scope); };
	host.lease = [](const AccountKey &account) -> std::shared_ptr<DesktopSyncCarryGate::Lease> {
		const auto gate = DesktopSyncCarryGateFor(account);
		return gate ? gate->carry() : nullptr;
	};
	host.quiesce = [weak] {
		const auto self = weak.get();
		if (const auto session = self ? self->_session.get() : nullptr) { QuiesceSyncChatPublishers(session); }
	};
	host.resolve = [weak](Fn<void(std::optional<DesktopSyncChatSetupSnapshot>)> done) {
		const auto self = weak.get();
		const auto session = self && self->current() ? self->_session.get() : nullptr;
		if (!session) { done({}); return; }
		ResolveSyncChatSetup(session, std::move(done));
	};
	host.inspect = [weak](PeerToken chat, Fn<void(std::optional<DesktopSyncChatPeer>)> done) {
		if (const auto self = weak.get()) { self->inspect(chat, std::move(done)); } else { done({}); }
	};
	host.provision = [weak](DesktopSyncChatMoveDraft draft, bool resume, DesktopSyncChatProvisionDone done) {
		if (const auto self = weak.get()) { self->provision(std::move(draft), resume, std::move(done)); } else { done({}, {}); }
	};
	host.resumeOpening = [](const DesktopSyncChatDiscoveryScope &scope) -> std::optional<DesktopSyncChatCarryResume> {
		const auto directory = CarryDirectory(scope);
		return directory ? DesktopSyncChatCarryResumeOpening(scope, *directory) : std::nullopt;
	};
	host.transport = [weak](const DesktopSyncChatSetupSnapshot &opening, DesktopSyncChatCarryRenewal renewal,
			Fn<bool()> current) {
		const auto self = weak.get();
		const auto session = self ? self->_session.get() : nullptr;
		return session ? NativeTransport(session, std::move(current), opening, std::move(renewal)) : DesktopSyncCarryTransport();
	};
	host.mark = [weak](const DesktopSyncChatSetupSnapshot &opening, Fn<bool()> current) {
		const auto self = weak.get();
		const auto session = self ? self->_session.get() : nullptr;
		return session ? NativeMarkPort(session, std::move(current), opening) : DesktopSyncChatMarkPort();
	};
	host.changed = [weak](const DesktopSyncChatCarryState &state) { if (const auto self = weak.get()) { self->changed(state); } };
	host.settled = [weak](DesktopSyncChatCarryKind kind) { if (const auto self = weak.get()) { self->settled(kind); } };
	return host;
}

void DesktopSyncChatCarryService::Impl::inspect(PeerToken chat, Fn<void(std::optional<DesktopSyncChatPeer>)> done) {
	const auto session = current() ? _session.get() : nullptr;
	if (!session) { done({}); return; }
	const auto id = ++_requestId;
	const auto weak = base::make_weak(this);
	_requests.emplace(id, std::make_unique<DesktopSyncChatPeerRequest>(session, _scope->origin, chat,
		[weak] { const auto self = weak.get(); return self && self->current(); },
		[weak, id, done = std::move(done)](std::optional<DesktopSyncChatPeer> peer) {
			crl::on_main([weak, id] { if (const auto self = weak.get()) { self->_requests.erase(id); } });
			done(std::move(peer));
		}));
}

// Provision-only create through the existing setup service. A Complete phase with
// `provisioned` hands back the create journal and fresh facts for the new channel; a held,
// limited or cancelled create answers nothing, and Retry resumes the same create journal.
void DesktopSyncChatCarryService::Impl::provision(DesktopSyncChatMoveDraft draft, bool resume,
		DesktopSyncChatProvisionDone done) {
	const auto session = current() ? _session.get() : nullptr;
	if (!session) { done({}, {}); return; }
	const auto weak = base::make_weak(this);
	const auto generation = ++_provisionGeneration;
	const auto answered = std::make_shared<bool>(false);
	_provision = std::make_unique<DesktopSyncChatSetupService>(session, draft.opening, draft.selection,
		[weak] { const auto self = weak.get(); return self && self->current(); },
		[weak, generation, answered, done](DesktopSyncChatSetupProgress progress) {
			const auto self = weak.get();
			if (!self || self->_provisionGeneration != generation || *answered) { return; }
			if (progress.provisioned && progress.journal && progress.journal->chat) {
				*answered = true;
				self->inspect(*progress.journal->chat, [journal = progress.journal, done](std::optional<DesktopSyncChatPeer> peer) {
					done(journal, std::move(peer));
				});
			} else if (progress.resumeAbsent || progress.phase == DesktopSyncChatCreatePhase::Held
				|| progress.phase == DesktopSyncChatCreatePhase::Limit
				|| progress.phase == DesktopSyncChatCreatePhase::Cancelled
				|| progress.phase == DesktopSyncChatCreatePhase::Complete) {
				*answered = true;
				done({}, {});
			}
		}, resume);
	_provision->start();
}

// Leaving the app or locking it stops the running operation like closing its window. A step
// still in flight keeps its journal, which resumes once the app is active and unlocked again.
void DesktopSyncChatCarryService::Impl::revoke(QString reason, bool resumeLater) {
	++_provisionGeneration;
	if (_provision) { _provision->cancel(); _provision.reset(); }
	_requests.clear();
	const auto unfinished = _owner->busy();
	_owner->close(std::move(reason));
	if (unfinished && resumeLater && ResumeActivated()) { _resumePending = true; }
}

void DesktopSyncChatCarryService::Impl::changed(const DesktopSyncChatCarryState &state) {
	if (state.finished && _provision) { ++_provisionGeneration; _provision->cancel(); _provision.reset(); }
	_changes.fire_copy(state);
}

void DesktopSyncChatCarryService::Impl::settled(DesktopSyncChatCarryKind kind) {
	if (auto then = std::exchange(_startup, nullptr)) { then(); return; }
	if (const auto session = _session.get(); session && kind != DesktopSyncChatCarryKind::None) {
		crl::on_main(session, [=] { EnsureSyncChatResolved(session, true); });
	}
}

void DesktopSyncChatCarryService::Impl::startup(Fn<void()> then) {
	if (!ResumeActivated()) { if (then) { then(); } return; }
	if (_owner->busy() || !_owner->resume()) {
		// Inactive, locked or busy: ordinary reconciliation reports any pending carry as held,
		// and the resume runs when the app is next active.
		_resumePending = !_owner->busy();
		if (then) { then(); }
		return;
	}
	_startup = std::move(then);
}

void DesktopSyncChatCarryService::Impl::resumeWhenActive() {
	if (!_resumePending || !current() || _owner->busy()) { return; }
	const auto weak = base::make_weak(this);
	crl::on_main([weak] {
		const auto self = weak.get();
		if (self && self->_resumePending && self->current() && !self->_owner->busy() && self->_owner->resume()) {
			self->_resumePending = false;
		}
	});
}

DesktopSyncChatCarryService::DesktopSyncChatCarryService(not_null<Main::Session*> session)
: _impl(std::make_unique<Impl>(session)) {
}

DesktopSyncChatCarryService::~DesktopSyncChatCarryService() = default;

DesktopSyncChatCarryOwner &DesktopSyncChatCarryService::owner() { return _impl->owner(); }
rpl::producer<DesktopSyncChatCarryState> DesktopSyncChatCarryService::changes() const { return _impl->changes(); }
void DesktopSyncChatCarryService::startup(Fn<void()> then) { _impl->startup(std::move(then)); }

namespace {

std::map<Main::Session*, std::unique_ptr<DesktopSyncChatCarryService>> Services;

}

DesktopSyncChatCarryService &SyncChatCarry(not_null<Main::Session*> session) {
	auto i = Services.find(session.get());
	if (i == Services.end()) {
		i = Services.emplace(session.get(), std::make_unique<DesktopSyncChatCarryService>(session)).first;
		session->lifetime().add([raw = session.get()] { Services.erase(raw); });
	}
	return *i->second;
}

void ResumeSyncChatCarry(not_null<Main::Session*> session, Fn<void()> then) {
	if (!ResumeActivated()) {
		if (then) { then(); }
		return;
	}
	SyncChatCarry(session).startup(std::move(then));
}

}

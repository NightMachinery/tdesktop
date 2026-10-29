#include "purple/purple_sync_chat_carry_service.h"

#include "base/weak_ptr.h"
#include "core/application.h"
#include "crl/crl_object_on_queue.h"
#include "crl/crl_on_main.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "purple/purple_sync_chat_service.h"
#include "purple/purple_sync_config_post.h"

#include <QtCore/QDir>

namespace Purple {

class DesktopSyncChatCarryService::Impl final : public base::has_weak_ptr {
public:
	Impl(not_null<Main::Session*> session, DesktopSyncChatDiscoveryScope opening, Fn<bool()> current,
		DesktopSyncCarryTransport transport, Fn<void(DesktopSyncCarryProgress)> progress)
	: _session(session), _opening(std::move(opening)), _current(std::move(current))
	, _transport(std::move(transport)), _progress(std::move(progress))
	, _admitted(std::make_shared<std::atomic<bool>>(true)) {
		const auto weak = base::make_weak(this);
		const auto changed = [weak] { if (const auto self = weak.get(); self && !self->current()) { self->cancel(); } };
		SettingsChanges() | rpl::on_next(changed, _lifetime);
		StateChanges() | rpl::on_next(changed, _lifetime);
		Core::App().appDeactivatedValue() | rpl::on_next([weak](bool inactive) {
			if (inactive) { if (const auto self=weak.get()) { self->cancel(); } }
		}, _lifetime);
		Core::App().passcodeLockChanges() | rpl::on_next([weak](bool locked) {
			if (locked) { if (const auto self=weak.get()) { self->cancel(); } }
		}, _lifetime);
		session->account().sessionChanges() | rpl::on_next([weak, session](Main::Session *value) {
			if (value != session) { if (const auto self = weak.get()) { self->cancel(); } }
		}, _lifetime);
		session->lifetime().add([weak] { if (const auto self = weak.get()) { self->cancel(); } });
	}
	~Impl() { cancel(); }
	bool current() const {
		return _admitted->load() && _session && _current && _current()
			&& QGuiApplication::applicationState() == Qt::ApplicationActive && !Core::App().passcodeLocked()
			&& CachedSyncChatSetupContextCurrent(_session.get(), _opening);
	}
	void start(std::optional<SyncCarryJournalStart> begin) {
		if (_started || !current()) { return; }
		const auto files = ResolveAccountFiles(_opening.origin.account, _opening.owner, AccountFileLayout::Desktop);
		const auto gate = DesktopSyncCarryGateFor(_opening.origin.account);
		const auto lease = gate ? gate->carry() : nullptr;
		if (!files || !lease) {
			if (_progress) { _progress({.reason = u"carry-writer-busy"_q, .storeStatus = SyncStoreStatus::LockBusy}); }
			return;
		}
		_started = true;
		const auto weak = base::make_weak(this);
		QuiesceSyncChatPublishers(_session.get());
		const auto self = weak.get();
		if (!self || !self->current()) { return; }
		const auto scope = SyncChatCarryStoreScope{_opening.origin.account, _opening.binding, _opening.loginBinding,
			QDir(_opening.root).filePath(DesktopSyncChatCacheFileName(_opening.origin.account))};
		_worker = std::make_unique<crl::object_on_queue<DesktopSyncChatCarry>>(
			QString(QDir(_opening.root).filePath(files->syncDirectory)), SyncChatCarryStoreScope(scope),
			DesktopSyncChatOrigin(_opening.origin), std::shared_ptr<std::atomic<bool>>(_admitted),
			std::shared_ptr<DesktopSyncCarryGate::Lease>(lease), [weak](DesktopSyncCarryProgress progress) {
				crl::on_main([weak, progress = std::move(progress)]() mutable {
					if (const auto self = weak.get()) { self->receive(std::move(progress)); }
				});
			}, SyncDurableFault());
		_worker->with([begin = std::move(begin)](DesktopSyncChatCarry &flow) mutable { flow.open(std::move(begin)); });
	}
	void receive(DesktopSyncCarryProgress progress) {
		if (!current()) { cancel(); return; }
		const auto command = progress.command;
		const auto terminal = progress.terminal || progress.absent;
		const auto weak = base::make_weak(this);
		if (_progress) { _progress(std::move(progress)); }
		const auto self = weak.get();
		if (!self || !self->current()) { return; }
		if (terminal) { self->cancel(); return; }
		if (!command || !_transport.send) { return; }
		const auto admitted = _admitted;
		const auto apply = _transport.applyUpdates;
		const auto applyCleanup = _transport.applyCleanup;
		_transport.send(*command, [weak, admitted, apply, applyCleanup, command = *command](DesktopSyncCarryReply reply) mutable {
			if (apply) { for (const auto &updates : reply.updates) { apply(updates); } }
			try {
				if (reply.cleanupResult && (!command.cleanupPermit || !applyCleanup
					|| !applyCleanup(command.cleanupPermit->route(),*reply.cleanupResult))) {
					reply.cleanupResult.reset(); reply.outcome=SyncCarryOutcome::Unknown;
				}
			} catch (...) { reply.cleanupResult.reset(); reply.outcome=SyncCarryOutcome::Unknown; }
			const auto self = weak.get();
			if (!self || !self->current() || !self->_worker) { return; }
			reply.command = command;
			if (reply.fresh) { reply.fresh->current = [admitted] { return admitted->load(); }; }
			self->_worker->with([reply = std::move(reply)](DesktopSyncChatCarry &flow) mutable { flow.complete(std::move(reply)); });
		});
	}
	void retry() {
		if (!current() || !_worker) { return; }
		_worker->with([](DesktopSyncChatCarry &flow) { flow.resume(); });
	}
	void cancel() {
		if (!_admitted->exchange(false)) { return; }
		const auto weak = base::make_weak(this);
		auto worker = std::move(_worker);
		if (worker) { worker->with([](DesktopSyncChatCarry &flow) { flow.cancel(); }); }
		if (_transport.cancel) { _transport.cancel(); }
	}
	void discardInterrupted(bool confirmed) {
		if (current() && _worker) { _worker->with([confirmed](DesktopSyncChatCarry &flow) { flow.discardInterrupted(confirmed); }); }
	}
	void skipCleanup(bool confirmed) {
		if (current() && _worker) { _worker->with([confirmed](DesktopSyncChatCarry &flow) { flow.skipCleanup(confirmed); }); }
	}
	void renew(DesktopSyncCarryCommand command, DesktopSyncCarryPermit permit, DesktopSyncCarryFresh facts,
			Fn<void(std::optional<DesktopSyncCarryPermit>)> ready) {
		if (!current() || !_worker) { ready(std::nullopt); return; }
		const auto admitted = _admitted;
		facts.current = [admitted] { return admitted->load(); };
		const auto weak = base::make_weak(this);
		_worker->with([weak, command = std::move(command), permit = std::move(permit), facts = std::move(facts), ready = std::move(ready)](DesktopSyncChatCarry &flow) mutable {
			auto renewed = flow.renew(command, permit, facts);
			crl::on_main([weak, renewed = std::move(renewed), ready = std::move(ready)]() mutable {
				const auto self = weak.get(); ready(self && self->current() ? std::move(renewed) : std::nullopt);
			});
		});
	}

	void beginCleanupRenew(DesktopSyncCarryCommand command, DesktopSyncCleanupPermit permit,
			Fn<void(std::optional<DesktopSyncCleanupRead>)> ready) {
		if (!current() || !_worker) { ready({}); return; }
		const auto weak=base::make_weak(this);
		_worker->with([weak,command=std::move(command),permit=std::move(permit),ready=std::move(ready)](DesktopSyncChatCarry &flow) mutable {
			auto read=flow.beginCleanupRenew(command,permit);
			crl::on_main([weak,read=std::move(read),ready=std::move(ready)]() mutable {
				const auto self=weak.get(); ready(self && self->current()?std::move(read):std::nullopt);
			});
		});
	}
	void renewCleanup(DesktopSyncCarryCommand command, DesktopSyncCleanupPermit permit, DesktopSyncCleanupRead read,
			DesktopSyncCarryReply reply, Fn<void(std::optional<DesktopSyncCleanupPermit>)> ready) {
		if (!current() || !_worker) { ready({}); return; }
		const auto weak=base::make_weak(this);
		_worker->with([weak,command=std::move(command),permit=std::move(permit),read=std::move(read),reply=std::move(reply),ready=std::move(ready)](DesktopSyncChatCarry &flow) mutable {
			auto next=flow.renewCleanup(command,permit,read,reply);
			crl::on_main([weak,next=std::move(next),ready=std::move(ready)]() mutable {
				const auto self=weak.get(); ready(self && self->current()?std::move(next):std::nullopt);
			});
		});
	}

private:
	base::weak_ptr<Main::Session> _session;
	DesktopSyncChatDiscoveryScope _opening;
	Fn<bool()> _current;
	DesktopSyncCarryTransport _transport;
	Fn<void(DesktopSyncCarryProgress)> _progress;
	std::shared_ptr<std::atomic<bool>> _admitted;
	std::unique_ptr<crl::object_on_queue<DesktopSyncChatCarry>> _worker;
	rpl::lifetime _lifetime;
	bool _started = false;

};

DesktopSyncChatCarryService::DesktopSyncChatCarryService(not_null<Main::Session*> session,
		DesktopSyncChatDiscoveryScope opening, Fn<bool()> current, DesktopSyncCarryTransport transport,
		Fn<void(DesktopSyncCarryProgress)> progress)
: _impl(std::make_unique<Impl>(session, std::move(opening), std::move(current), std::move(transport), std::move(progress))) {}
DesktopSyncChatCarryService::~DesktopSyncChatCarryService() = default;
void DesktopSyncChatCarryService::start(std::optional<SyncCarryJournalStart> begin) { _impl->start(std::move(begin)); }
void DesktopSyncChatCarryService::retry() { _impl->retry(); }
void DesktopSyncChatCarryService::cancel() { _impl->cancel(); }
void DesktopSyncChatCarryService::discardInterrupted(bool confirmed) { _impl->discardInterrupted(confirmed); }
void DesktopSyncChatCarryService::skipCleanup(bool confirmed) { _impl->skipCleanup(confirmed); }
void DesktopSyncChatCarryService::renew(const DesktopSyncCarryCommand &command, const DesktopSyncCarryPermit &permit,
		DesktopSyncCarryFresh fresh, Fn<void(std::optional<DesktopSyncCarryPermit>)> ready) { _impl->renew(command, permit, std::move(fresh), std::move(ready)); }

void DesktopSyncChatCarryService::beginCleanupRenew(DesktopSyncCarryCommand command, DesktopSyncCleanupPermit permit,
		Fn<void(std::optional<DesktopSyncCleanupRead>)> ready) { _impl->beginCleanupRenew(std::move(command),std::move(permit),std::move(ready)); }
void DesktopSyncChatCarryService::renewCleanup(DesktopSyncCarryCommand command, DesktopSyncCleanupPermit permit,
		DesktopSyncCleanupRead read, DesktopSyncCarryReply reply, Fn<void(std::optional<DesktopSyncCleanupPermit>)> ready) {
	_impl->renewCleanup(std::move(command),std::move(permit),std::move(read),std::move(reply),std::move(ready));
}

}

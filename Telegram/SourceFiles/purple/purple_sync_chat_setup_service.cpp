#include "purple/purple_sync_chat_setup_service.h"

#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "crl/crl_object_on_queue.h"
#include "crl/crl_on_main.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "purple/purple_sync_chat_service.h"
#include "purple/purple_sync_chat_carry_gate.h"
#include "purple/purple_sync_chat_setup_transport.h"

#include <QtCore/QDir>

namespace Purple {

class DesktopSyncChatSetupService::Impl final : public base::has_weak_ptr {
public:
	Impl(not_null<Main::Session*> session, DesktopSyncChatSetupSnapshot opening,
		DesktopSyncChatSetupSelection selection, Fn<bool()> current,
		Fn<void(DesktopSyncChatSetupProgress)> progress, bool resumeOnly);
	~Impl();
	bool current() const;
	void start();
	void retry();
	void useSelf();
	void reconsent(DesktopSyncChatConsentOffer offer);
	void cancel(bool notify);
	void receive(DesktopSyncChatSetupProgress progress);
	void finishResumeLookup();

private:
	base::weak_ptr<Main::Session> _session;
	DesktopSyncChatSetupSnapshot _opening;
	DesktopSyncChatSetupSelection _selection;
	Fn<bool()> _current;
	Fn<void(DesktopSyncChatSetupProgress)> _progress;
	std::shared_ptr<std::atomic<bool>> _admitted;
	std::unique_ptr<crl::object_on_queue<DesktopSyncChatSetupFlow>> _worker;
	std::unique_ptr<DesktopSyncChatSetupTransport> _transport;
	DesktopSyncChatSetupProgress _last;
	rpl::lifetime _lifetime;
	bool _started = false;
	bool _resumeOnly = false;

};

DesktopSyncChatSetupService::Impl::Impl(not_null<Main::Session*> session,
		DesktopSyncChatSetupSnapshot opening, DesktopSyncChatSetupSelection selection,
		Fn<bool()> current, Fn<void(DesktopSyncChatSetupProgress)> progress, bool resumeOnly)
: _session(session)
, _opening(std::move(opening))
, _selection(std::move(selection))
, _current(std::move(current))
, _progress(std::move(progress))
, _admitted(std::make_shared<std::atomic<bool>>(true))
, _resumeOnly(resumeOnly) {
	const auto weak = base::make_weak(this);
	const auto changed = [weak] {
		if (const auto self = weak.get(); self && !self->current()) {
			self->cancel(true);
		}
	};
	SettingsChanges() | rpl::on_next(changed, _lifetime);
	SyncChatPublisherQuiescence() | rpl::on_next([weak, expected = session.get()](Main::Session *value) {
		if (value == expected) { if (const auto self = weak.get()) { self->cancel(true); } }
	}, _lifetime);
	StateChanges() | rpl::on_next(changed, _lifetime);
	session->account().sessionChanges() | rpl::on_next([weak, session](Main::Session *value) {
		if (value != session) {
			if (const auto self = weak.get()) { self->cancel(true); }
		}
	}, _lifetime);
	session->lifetime().add([weak] {
		if (const auto self = weak.get()) { self->cancel(false); }
	});
}

DesktopSyncChatSetupService::Impl::~Impl() {
	cancel(false);
}

bool DesktopSyncChatSetupService::Impl::current() const {
	return _session && !DesktopSyncCarryGateFor(_opening.scope.origin.account)->postingHeld()
		&& _admitted->load() && _current && _current()
		&& SyncChatSetupContextCurrent(_session.get(), _opening.scope);
}

void DesktopSyncChatSetupService::Impl::start() {
	if (_started || !current()) {
		return;
	}
	_started = true;
	const auto &scope = _opening.scope;
	const auto files = ResolveAccountFiles(scope.origin.account, scope.owner, AccountFileLayout::Desktop);
	if (!files) { cancel(true); return; }
	const auto directory = QDir(scope.root).filePath(files->syncDirectory);
	const auto statePath = QDir(scope.root).filePath(files->syncStore);
	const auto weak = base::make_weak(this);
	const auto current = [weak] {
		const auto self = weak.get(); return self && self->current();
	};
	_transport = std::make_unique<DesktopSyncChatSetupTransport>(_session.get(), scope,
		DesktopSyncChatOperation{ directory, scope.origin.account, current }, current,
		[weak](DesktopSyncChatCreateCommand command, DesktopSyncChatSetupResumeProof proof,
				Fn<void(bool)> ready) {
			const auto self = weak.get();
			if (!self || !self->current() || !self->_worker) { ready(false); return; }
			self->_worker->with([weak, command = std::move(command), proof = std::move(proof),
					ready = std::move(ready)](DesktopSyncChatSetupFlow &flow) {
				const auto allowed = flow.renew(command, proof, int64_t(base::unixtime::now()));
				crl::on_main([weak, ready = std::move(ready), allowed] {
					const auto self = weak.get();
					ready(allowed && self && self->current());
				});
			});
		});
	_worker = std::make_unique<crl::object_on_queue<DesktopSyncChatSetupFlow>>(
		DesktopSyncChatSetupSnapshot(_opening),
		DesktopSyncChatSetupSelection(_selection),
		QString(directory), QString(statePath),
		std::shared_ptr<std::atomic<bool>>(_admitted),
		[weak](DesktopSyncChatSetupProgress progress) {
			crl::on_main([weak, progress = std::move(progress)]() mutable {
				if (const auto self = weak.get()) { self->receive(std::move(progress)); }
			});
		}, bool(_resumeOnly));
	_worker->with([now = int64_t(base::unixtime::now())](DesktopSyncChatSetupFlow &flow) {
		flow.open(now);
	});
}

void DesktopSyncChatSetupService::Impl::receive(DesktopSyncChatSetupProgress progress) {
	if (!current()) { cancel(true); return; }
	_last = std::move(progress);
	if (_last.resumeAbsent) { finishResumeLookup(); }
	const auto command = _last.command;
	const auto weak = base::make_weak(this);
	if (_progress) { _progress(_last); }
	const auto self = weak.get();
	if (!self || !self->current() || !command || !self->_transport) {
		return;
	}
	self->_transport->send(*command, [weak](DesktopSyncChatCreateReply reply) {
		const auto self = weak.get();
		if (!self || !self->current() || !self->_worker) { return; }
		self->_worker->with([reply = std::move(reply), now = int64_t(base::unixtime::now())](
				DesktopSyncChatSetupFlow &flow) { flow.complete(reply, now); });
	});
}

void DesktopSyncChatSetupService::Impl::finishResumeLookup() {
	_admitted->store(false);
	if (_transport) { _transport->cancel(); }
	_worker.reset();
}

void DesktopSyncChatSetupService::Impl::retry() {
	if (!current() || !_worker || _last.phase == DesktopSyncChatCreatePhase::Running
		|| _last.phase == DesktopSyncChatCreatePhase::Complete) { return; }
	_transport->cancel();
	_worker->with([now = int64_t(base::unixtime::now())](DesktopSyncChatSetupFlow &flow) {
		flow.resume(now);
	});
}

void DesktopSyncChatSetupService::Impl::useSelf() {
	if (!current() || !_worker || _last.phase != DesktopSyncChatCreatePhase::Limit) { return; }
	_transport->cancel();
	_worker->with([now = int64_t(base::unixtime::now())](DesktopSyncChatSetupFlow &flow) {
		flow.useSelf(now);
	});
}

void DesktopSyncChatSetupService::Impl::reconsent(DesktopSyncChatConsentOffer offer) {
	if (!current() || !_worker || !_last.consent) { return; }
	_transport->cancel();
	_worker->with([offer = std::move(offer), now = int64_t(base::unixtime::now())](
			DesktopSyncChatSetupFlow &flow) { flow.reconsent(offer, now); });
}

void DesktopSyncChatSetupService::Impl::cancel(bool notify) {
	if (!_admitted->exchange(false)) { return; }
	if (_transport) { _transport->cancel(); }
	if (_worker) {
		_worker->with([](DesktopSyncChatSetupFlow &flow) { flow.cancel(); });
	}
	if (notify && _progress) {
		auto status = _last;
		status.phase = DesktopSyncChatCreatePhase::Cancelled;
		status.reason = u"setup-context-held"_q;
		status.command.reset();
		_progress(std::move(status));
	}
}

DesktopSyncChatSetupService::DesktopSyncChatSetupService(
		not_null<Main::Session*> session, DesktopSyncChatSetupSnapshot opening,
		DesktopSyncChatSetupSelection selection, Fn<bool()> current,
		Fn<void(DesktopSyncChatSetupProgress)> progress, bool resumeOnly)
: _impl(std::make_unique<Impl>(session, std::move(opening), std::move(selection),
	std::move(current), std::move(progress), resumeOnly)) {
}

DesktopSyncChatSetupService::~DesktopSyncChatSetupService() = default;
void DesktopSyncChatSetupService::start() { _impl->start(); }
void DesktopSyncChatSetupService::retry() { _impl->retry(); }
void DesktopSyncChatSetupService::useSelf() { _impl->useSelf(); }
void DesktopSyncChatSetupService::reconsent(const DesktopSyncChatConsentOffer &offer) { _impl->reconsent(offer); }
void DesktopSyncChatSetupService::cancel() { _impl->cancel(true); }

}

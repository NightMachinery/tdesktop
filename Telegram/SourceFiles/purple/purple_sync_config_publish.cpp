#include "purple/purple_sync_config_publish.h"

#include "base/unixtime.h"
#include "core/application.h"
#include "crl/crl_on_main.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "purple/purple_sync_account_binding.h"
#include "purple/purple_sync_chat_write.h"

#include <QtGui/QGuiApplication>

namespace Purple {

SyncConfigPublish::SyncConfigPublish(
		Main::Account &account,
		Main::Session &session,
		SyncAccountInventoryResult inventory,
		SyncConfigSendQueue scanQueue,
		uint64_t scanPosts,
		Fn<void(SyncConfigPublishResult)> finished,
		SyncConfigPublishRequest request)
: _account(base::make_weak(&account))
, _session(base::make_weak(&session))
, _inventory(std::move(inventory))
, _scanQueue(scanQueue)
, _scanPosts(scanPosts)
, _finished(std::move(finished))
, _request(std::move(request))
, _routing(CaptureAccountSyncRouting(&session))
, _bindingToken(AccountSyncBindingToken(account))
, _admitted(std::make_shared<std::atomic<bool>>(true)) {
	const auto weak = base::make_weak(this);
	const auto changed = [weak] {
		const auto self = weak.get();
		if (self && !self->_done && !self->AccountAvailable()) { self->Cancel(); }
	};
	SettingsChanges() | rpl::on_next(changed, _lifetime);
	SyncChatPublisherQuiescence() | rpl::on_next([weak, expected = &session](Main::Session *value) {
		if (value == expected) { if (const auto self = weak.get()) { self->Cancel(); } }
	}, _lifetime);
	Core::App().appDeactivatedValue() | rpl::on_next([weak](bool inactive) {
		const auto self = weak.get();
		if (inactive && self && self->_started && !self->_done) { self->Cancel(); }
	}, _lifetime);
	StateChanges() | rpl::on_next(changed, _lifetime);
	Core::App().passcodeLockChanges() | rpl::on_next([weak](bool locked) {
		if (locked) { if (const auto self = weak.get()) { self->Cancel(); } }
	}, _lifetime);
	account.sessionChanges() | rpl::on_next([weak, expected = &session](Main::Session *live) {
		if (live != expected) {
			if (const auto self = weak.get()) { self->Cancel(); }
		}
	}, _lifetime);
	session.lifetime().add([weak] {
		if (const auto self = weak.get()) { self->Cancel(); }
	});
}

SyncConfigPublish::SyncConfigPublish(
		Main::Account &account,
		Main::Session &session,
		DesktopSyncChatInventory inventory,
		SyncConfigSendQueue scanQueue,
		uint64_t scanPosts,
		Fn<void(SyncConfigPublishResult)> finished,
		SyncConfigPublishRequest request,
		std::optional<DesktopSyncChatSetupSnapshot> setup,
		Fn<bool()> current, bool initializeOnly)
: SyncConfigPublish(account, session, std::move(inventory.inventory),
	scanQueue, scanPosts, std::move(finished), std::move(request)) {
	_inventoryRoute = std::move(inventory.route);
	_setup = std::move(setup);
	_current = std::move(current);
	_initializeOnly = initializeOnly;
}

SyncConfigPublish::~SyncConfigPublish() {
	_admitted->store(false);
	_done = true;
	if (_post) { _post->Cancel(); }
	if (_worker && _carryWriter) {
		_worker->with([lease = std::move(_carryWriter)](SyncConfigPublishFlow &) {});
	} else { _carryWriter.reset(); }
	_worker.reset();
}

bool SyncConfigPublish::AccountAvailable() const {
	const auto account = _account.get();
	const auto session = _session.get();
	return !_done && _admitted->load() && account && session && _routing
		&& (!_started || (_carryWriter && _carryWriter->current()))
		&& QGuiApplication::applicationState() == Qt::ApplicationActive
		&& !Core::App().passcodeLocked()
		&& AccountSyncRoutingCurrent(session, *_routing)
		&& !account->loggingOut() && account->maybeSession() == session
		&& SessionAccountKey(session).userId == _inventory.accountUserId
		&& AccountSyncBindingToken(*account) == _bindingToken
		&& (!_current || _current())
		&& (!_setup || CachedSyncChatSetupContextCurrent(session, _setup->scope))
		&& (!_inventoryRoute || SyncChatIoBelongsToSession(session, *_inventoryRoute));
}

void SyncConfigPublish::Start() {
	if (_started || _done) { return; }
	const auto sessionForGate = _session.get();
	if (!sessionForGate) { Finish({SyncConfigPublishStatus::AccountUnavailable}); return; }
	_carryWriter = DesktopSyncCarryGateFor(SessionAccountKey(sessionForGate))->writer();
	_started = true;
	if (!AccountAvailable()) { Finish({ SyncConfigPublishStatus::AccountUnavailable }); return; }
	const auto session = _session.get();
	const auto chat = _inventoryRoute ? _inventoryRoute->peer.chat : PeerToken{};
	auto input = SyncConfigPublishDiskInput();
	input.routing = *_routing;
	input.account = SessionAccountKey(session);
	input.chat = chat;
	input.binding = _bindingToken;
	input.setup = _setup;
	input.inventory = _inventory;
	input.request = _request;
	input.writer = SyncWriter();
	input.now = int64_t(base::unixtime::now());
	input.initializeOnly = _initializeOnly;
	input.queue = _scanQueue == SyncConfigSendQueue::HoldsSyncRecord
		|| SyncConfigPostsStarted(session, chat) != _scanPosts
		? SyncConfigSendQueue::HoldsSyncRecord : SyncConfigSendQueueOf(session, chat);
	for (const auto &entry : Core::App().domain().accounts()) {
		if (entry.account.get() == _account.get()) { continue; }
		const auto live = entry.account->maybeSession();
		const auto token = AccountSyncBindingToken(*entry.account);
		if (!live && !token.isEmpty()) { Finish({ SyncConfigPublishStatus::NeedsReview }); return; }
		if (live) { input.otherPreferences.push_back({ SessionAccountKey(live), token }); }
	}
	const auto weak = base::make_weak(this);
	_worker = std::make_unique<crl::object_on_queue<SyncConfigPublishFlow>>(
		std::move(input), std::shared_ptr<std::atomic<bool>>(_admitted),
		[weak](SyncConfigPublishDiskReply reply) {
			crl::on_main([weak, reply = std::move(reply)]() mutable {
				if (const auto self = weak.get(); self && !self->_done) { self->Receive(std::move(reply)); }
			});
		});
	_worker->with([](SyncConfigPublishFlow &flow) { flow.start(); });
}

void SyncConfigPublish::Receive(SyncConfigPublishDiskReply reply) {
	if (!AccountAvailable()) { Cancel(); return; }
	if (reply.step == SyncConfigPublishDiskStep::Finished) { Finish(reply.result); return; }
	if (reply.step == SyncConfigPublishDiskStep::Binding) {
		if (!reply.initial || !StoreAccountSyncBinding(*reply.initial, *_account.get())) {
			Finish({ SyncConfigPublishStatus::AccountUnbound }); return;
		}
		_bindingToken = AccountSyncBindingToken(*_account.get());
		if (_setup) { _setup->scope.binding = _bindingToken; }
		_worker->with([token = _bindingToken](SyncConfigPublishFlow &flow) { flow.bindingCommitted(token); });
		return;
	}
	_directory = std::move(reply.directory);
	StartPost(reply.staged);
}

void SyncConfigPublish::Renew(Fn<void(bool)> ready) {
	if (!AccountAvailable() || !_worker) { ready(false); return; }
	const auto weak = base::make_weak(this);
	_worker->with([weak, ready = std::move(ready)](SyncConfigPublishFlow &flow) {
		const auto allowed = flow.authorize();
		crl::on_main([weak, ready = std::move(ready), allowed] {
			const auto self = weak.get();
			ready(allowed && self && self->AccountAvailable());
		});
	});
}

void SyncConfigPublish::StartPost(const QByteArray &staged) {
	if (!AccountAvailable()) { Cancel(); return; }
	const auto chat = _inventoryRoute ? _inventoryRoute->peer.chat : PeerToken{};
	if (SyncConfigPostsStarted(_session.get(), chat) != _scanPosts
		|| SyncConfigSendQueueOf(_session.get(), chat) != SyncConfigSendQueue::Empty) {
		Finish({ SyncConfigPublishStatus::StillSending }); return;
	}
	const auto weak = base::make_weak(this);
	const auto current = [weak] {
		const auto self = weak.get(); return self && self->AccountAvailable();
	};
	const auto operation = DesktopSyncChatOperation{
		.directory = _directory,
		.account = SessionAccountKey(_session.get()),
		.current = current,
		.routing = _routing,
		.authorize = [weak](Fn<void(bool)> ready) {
			if (const auto self = weak.get()) { self->Renew(std::move(ready)); }
			else { ready(false); }
		},
	};
	const auto finished = [weak](SyncConfigPostResult result) {
		if (const auto self = weak.get(); self && !self->_done) { self->OnPost(std::move(result)); }
	};
	if (_inventoryRoute) {
		_post = std::make_unique<SyncConfigPost>(
			_session.get(), *_inventoryRoute, staged, finished, current, operation);
	} else {
		_post = std::make_unique<SyncConfigPost>(
			_session.get(), staged, finished, current, operation);
	}
	_posted = true;
	_post->Start();
}

void SyncConfigPublish::OnPost(SyncConfigPostResult result) {
	if (!AccountAvailable() || !_worker) { Finish({ SyncConfigPublishStatus::OutcomeUnknown }); return; }
	if (result.status == SyncConfigPostStatus::Confirmed) {
		_worker->with([bytes = std::move(result.readback), id = int32_t(result.messageId.bare)](
				SyncConfigPublishFlow &flow) { flow.confirm(bytes, id); });
	} else {
		Finish({ result.status == SyncConfigPostStatus::NeedsReview
			|| result.status == SyncConfigPostStatus::InvalidRecord
			? SyncConfigPublishStatus::NeedsReview : SyncConfigPublishStatus::OutcomeUnknown });
	}
}

void SyncConfigPublish::Cancel() {
	if (_done) { return; }
	Finish({ _posted ? SyncConfigPublishStatus::OutcomeUnknown : SyncConfigPublishStatus::Cancelled });
}

void SyncConfigPublish::Finish(SyncConfigPublishResult result) {
	if (_done) { return; }
	_done = true;
	_admitted->store(false);
	if (_post) { _post->Cancel(); }
	if (_worker && _carryWriter) {
		_worker->with([lease = std::move(_carryWriter)](SyncConfigPublishFlow &) {});
	} else { _carryWriter.reset(); }
	_worker.reset();
	if (auto finished = std::move(_finished)) { finished(std::move(result)); }
}

}

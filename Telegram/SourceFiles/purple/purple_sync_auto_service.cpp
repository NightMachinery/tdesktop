#include "purple/purple_sync_auto_service.h"

#include "purple/purple_sync_chat_service.h"

#include "apiwrap.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "crl/crl_on_main.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "purple/purple_sync_account_binding.h"
#include "purple/purple_sync_account_inventory.h"
#include "purple/purple_sync_account_recovery.h"
#include "purple/purple_sync_auto_store.h"
#include "purple/purple_sync_auto_transport.h"
#include "purple/purple_sync_config_apply.h"
#include "purple/purple_sync_config_post.h"
#include "purple/purple_sync_config_review.h"
#include "purple/purple_sync_config_upload.h"
#include "purple/purple_sync_request_sender.h"
#include "purple/purple_sync_setup_box.h"

#include <QtCore/QFileInfo>
#include <QtCore/QUuid>

#include <algorithm>
#include <map>
#include <set>

namespace Purple {
namespace {

constexpr auto kSaveDelay = crl::time(5000);
rpl::event_stream<> Changes;

class AutoSession final : public base::has_weak_ptr {
public:
	explicit AutoSession(not_null<Main::Session*> session);
	~AutoSession();
	bool Event(SyncAutoEvent event);
	SyncAutoReason FailureReason() const { return _failureReason; }
	void Saved();
	void Replaced();
	bool Running(const SyncAutoRecord &record) const {
		return _inFlight && _store && record.active && record.session == _sessionId && record.ticket == _plan.ticket;
	}

private:
	bool Current() const;
	bool Open();
	std::optional<SyncAutoFacts> Facts();
	bool Accept(const SyncAutoPlan &plan);
	void Hold(SyncAutoReason reason);
	void Run(SyncAutoPlan plan);
	void Complete(const SyncAutoPlan &plan, SyncAutoOutcome outcome,
		std::optional<SyncAutoInventory> inventory = std::nullopt);
	void Check(const SyncAutoPlan &plan);
	void ReadNext();
	void Send(const SyncAutoPlan &plan);
	[[nodiscard]] bool EffectCurrent(const SyncAutoPlan &plan) const;
	[[nodiscard]] DesktopSyncChatOperation OperationFor(const SyncAutoPlan &plan);
	void Delete(const SyncAutoPlan &plan);
	void ReadForEffect(const SyncAutoPlan &plan, Fn<void(SyncAutoServerRead)> done);
	void RenewEffect(const SyncAutoPlan &plan, Fn<void(bool)> ready);
	void Stop();
	void RetryLater();

	base::weak_ptr<Main::Session> _session;
	AccountKey _account;
	QString _sessionId;
	QByteArray _token;
	std::optional<AccountSettingsOpenTarget> _opening;
	std::unique_ptr<DesktopSyncAutoStore> _store;
	std::unique_ptr<SyncAccountInventory> _scan;
	std::unique_ptr<DesktopSyncExactRead> _exact;
	std::unique_ptr<SyncConfigPost> _post;
	std::shared_ptr<SyncConfigUpload> _upload;
	std::unique_ptr<SyncRequestSender> _api;
	SyncAutoPlan _plan;
	std::optional<AccountTransferReview> _factsReview;
	std::optional<AccountTransferReview> _applyReview;
	SyncAccountInventoryResult _inventory;
	SyncAutoInventory _snapshot;
	std::vector<int32_t> _readIds;
	size_t _readIndex = 0;
	uint64_t _scanPosts = 0;
	SyncConfigSendQueue _scanQueue = SyncConfigSendQueue::Empty;
	bool _inFlight = false;
	SyncAutoReason _failureReason = SyncAutoReason::None;
	base::Timer _retry;
	base::Timer _save;
	QString _savedFingerprint;
	QString _pendingFingerprint;
	std::optional<AccountSettingsOpenTarget> _pendingOpening;
	QByteArray _pendingIdentity;
	QByteArray _savedIdentity;

};

std::map<Main::Session*, std::unique_ptr<AutoSession>> Sessions;

AutoSession &For(not_null<Main::Session*> session) {
	auto &value = Sessions[session.get()];
	if (!value) {
		value = std::make_unique<AutoSession>(session);
		const auto key = session.get();
		session->lifetime().add([key] { Sessions.erase(key); });
	}
	return *value;
}

AutoSession::AutoSession(not_null<Main::Session*> session)
: _session(base::make_weak(session.get()))
, _account(SessionAccountKey(session))
, _sessionId(QUuid::createUuid().toString(QUuid::WithoutBraces))
, _retry([=] { Event(SyncAutoEvent::Retry); })
, _save([=] {
	const auto session = _session.get();
	const auto target = session ? AccountSyncTarget(session) : std::nullopt;
	const auto opening = session ? AccountSettingsOpenTargetFor(session, SettingsOpenRole::Own) : std::nullopt;
	if (target && opening && _pendingOpening
		&& SameDesktopSyncAutoContext(*_pendingOpening, *opening)
		&& SyncPathIdentity(target->file.path) == _pendingIdentity
		&& SettingsFingerprint(target->file.input.bytes) == _pendingFingerprint) {
		_savedFingerprint = _pendingFingerprint;
		_savedIdentity = SyncPathIdentity(target->file.path);
		_pendingFingerprint.clear();
		_pendingOpening.reset();
		_pendingIdentity.clear();
		Event(SyncAutoEvent::LocalSaved);
	} else {
		Replaced();
	}
}) {
}

AutoSession::~AutoSession() { Stop(); }

bool AutoSession::Current() const {
	const auto session = _session.get();
	if (!session || session->account().loggingOut() || session->account().maybeSession() != session
		|| SessionAccountKey(session) != _account || !_opening
		|| AccountSyncBindingToken(session->account()) != _token) { return false; }
	const auto opening = AccountSettingsOpenTargetFor(session, SettingsOpenRole::Own);
	return opening && SameDesktopSyncAutoContext(*_opening, *opening);
}

bool AutoSession::Open() {
	if (_store) { return Current() && _store->Current(); }
	const auto session = _session.get();
	if (!session || session->account().loggingOut() || session->account().maybeSession() != session) { return false; }
	_opening = AccountSettingsOpenTargetFor(session, SettingsOpenRole::Own);
	const auto target = AccountSyncInspectionTarget(session);
	if (!_opening || !target || !SettingsStartupOwner()) { return false; }
	const auto recovery = InspectSyncAccountRecovery(*session);
	if (!recovery || recovery->journalPresent || recovery->plan.status != SyncAccountRecoveryStatus::Bound) { return false; }
	_token = AccountSyncBindingToken(session->account());
	const auto weak = base::make_weak(this);
	_store = std::make_unique<DesktopSyncAutoStore>(target->files, _account, _token, [weak] {
		const auto self = weak.get();
		return self && self->Current();
	});
	if (!_store->Open()) { _store.reset(); return false; }
	return true;
}

std::optional<SyncAutoFacts> AutoSession::Facts() {
	if (!_store || !_store->Current() || !_store->local().state()) { return std::nullopt; }
	auto facts = SyncAutoFacts();
	facts.account = facts.fileAccount = _account;
	facts.session = _sessionId;
	facts.signedIn = Current();
	facts.ownerAnswered = SettingsStartupOwner().has_value();
	facts.bindingToken = _token;
	facts.inFlightSession = _inFlight ? _sessionId : QString();
	facts.state = *_store->local().state();
	facts.currentDevice = facts.state->createdDevice;
	facts.chat = facts.state->chat;
	facts.chatResolved = facts.state->version == 1
		&& facts.chat.kind == PeerTokenKind::Self && SyncChatSelfResolved(_session.get());
	facts.chatWritable = true;
	facts.local = ReadSyncSettingsFile(_store->files());
	facts.savedFingerprint = SyncPathIdentity(_store->files().settings) == _savedIdentity
		? _savedFingerprint : QString();
	facts.saveDebounceFinished = !_save.isActive();
	facts.queue = SyncConfigSendQueueOf(not_null{ _session.get() });
	if (_inFlight && (_scanQueue != SyncConfigSendQueue::Empty
		|| SyncConfigPostsStarted(not_null{ _session.get() }) != _scanPosts)) {
		facts.queue = SyncConfigSendQueue::HoldsSyncRecord;
	}
	facts.editSupported = true;
	facts.writer = SyncWriter();
	if (facts.state->config.pendingSeq) {
		const auto pending = _store->local().ReadPendingConfig();
		if (!pending || !_store->local().state()) { return std::nullopt; }
		facts.stagedRecord = pending.staged;
	}
	_factsReview = ReviewAccountTransfer(not_null{ _session.get() }, facts.local.text);
	if (const auto &review = _factsReview) {
		facts.impactContext = review->context;
		facts.impactContextComplete = !review->usingGood;
		facts.proposedWorkMode = review->context.workMode;
	}
	return facts;
}

void AutoSession::Saved() {
	const auto session = _session.get();
	const auto target = session ? AccountSyncTarget(session) : std::nullopt;
	const auto opening = session ? AccountSettingsOpenTargetFor(session, SettingsOpenRole::Own) : std::nullopt;
	if (!target || !opening) { return; }
	_pendingOpening = opening;
	_pendingIdentity = SyncPathIdentity(target->file.path);
	_pendingFingerprint = SettingsFingerprint(target->file.input.bytes);
	_save.callOnce(kSaveDelay);
	Event(SyncAutoEvent::LocalSaved);
}

void AutoSession::Replaced() {
	_save.cancel();
	_pendingFingerprint.clear();
	_pendingOpening.reset();
	_pendingIdentity.clear();
	_savedFingerprint.clear();
	_savedIdentity.clear();
}

bool AutoSession::Event(SyncAutoEvent event) {
	const auto session = _session.get();
	if (!session) { Stop(); return false; }
	const auto target = AccountSyncInspectionTarget(session);
	if (!target) { Stop(); return false; }
	const auto read = ReadDesktopSyncAuto(target->files, _account);
	if (!read.record) { Stop(); return false; }
	if (!read.record->enabled && event != SyncAutoEvent::Enable && event != SyncAutoEvent::Disable) { return true; }
	if (event != SyncAutoEvent::Disable && !SyncChatSelfResolved(session)) {
		Stop();
		EnsureSyncChatResolved(session);
		return false;
	}
	if (!Open()) { Stop(); return false; }
	const auto facts = Facts();
	if (!facts) { Hold(SyncAutoReason::StateChanged); return false; }
	const auto plan = _store->Transition(*facts, event, int64_t(base::unixtime::now()));
	if (event == SyncAutoEvent::Disable) {
		_save.cancel();
		_savedFingerprint.clear();
		_savedIdentity.clear();
		_pendingFingerprint.clear();
		_pendingOpening.reset();
		_pendingIdentity.clear();
		Stop();
		return !plan.record.enabled;
	}
	if (plan.action == SyncAutoAction::NoOp && _inFlight && _store->record().active) {
		Changes.fire({});
		return true;
	}
	Run(plan);
	return true;
}

bool AutoSession::Accept(const SyncAutoPlan &plan) {
	if (!_inFlight || !_store || _plan.ticket != plan.ticket || _sessionId != plan.record.session) { return false; }
	const auto completion = _store->CheckCompletion(plan, _sessionId);
	if (completion == DesktopSyncAutoCompletion::Obsolete) { return false; }
	if (!SyncChatSelfResolved(_session.get())) {
		Hold(SyncAutoReason::SyncChatNotResolved);
		return false;
	}
	if (completion != DesktopSyncAutoCompletion::Ready) {
		Hold(completion == DesktopSyncAutoCompletion::ContextChanged
			? SyncAutoReason::OwnershipChanged : SyncAutoReason::StateChanged);
		return false;
	}
	return true;
}

void AutoSession::Hold(SyncAutoReason reason) {
	_failureReason = reason;
	if (_store) { static_cast<void>(_store->HoldCheckpoint(_plan, reason)); }
	Stop();
}

void AutoSession::Stop() {
	_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	_inFlight = false;
	_retry.cancel();
	_scan.reset();
	_exact.reset();
	_post.reset();
	if (_upload) {
		_upload->Cancel();
	}
	_upload.reset();
	_api.reset();
	_store.reset();
	_opening.reset();
	Changes.fire({});
}

void AutoSession::RetryLater() {
	const auto session = _session.get();
	if (!session) { return; }
	const auto target = AccountSyncInspectionTarget(session);
	const auto read = target ? ReadDesktopSyncAuto(target->files, _account) : DesktopSyncAutoRead();
	if (read.record && read.record->enabled && read.record->retryAt && !read.record->runStopped) {
		const auto seconds = std::max(int64_t(0), read.record->retryAt - int64_t(base::unixtime::now()));
		_retry.callOnce(crl::time(seconds * 1000));
	}
}

void AutoSession::Run(SyncAutoPlan plan) {
	if (plan.action == SyncAutoAction::NoOp || plan.action == SyncAutoAction::Hold
		|| plan.action == SyncAutoAction::RetryAt) {
		const auto retry = plan.action != SyncAutoAction::Hold;
		Stop();
		if (retry) { RetryLater(); }
		return;
	}
	_failureReason = SyncAutoReason::None;
	_plan = std::move(plan);
	if (_plan.action == SyncAutoAction::AutoApply) {
		_applyReview = _factsReview;
		if (_applyReview) {
			_applyReview->context.proposed = { _plan.bytes, true };
			_applyReview->impact = AnalyzeConfigChange(_applyReview->context);
		}
	}
	_inFlight = true;
	Changes.fire({});
	const auto weak = base::make_weak(this);
	crl::on_main([weak, ticket = _plan.ticket, owner = _sessionId] {
		const auto self = weak.get();
		if (!self || self->_sessionId != owner || self->_plan.ticket != ticket) { return; }
		if (!self->Accept(self->_plan)) { return; }
		if (!self->_store->Authorized(self->_plan)) { self->Hold(SyncAutoReason::StateChanged); return; }
		const auto plan = self->_plan;
		switch (plan.action) {
		case SyncAutoAction::Check:
			self->Check(plan);
			break;
		case SyncAutoAction::Reconcile:
			if (!plan.commitOnly) { self->Check(plan); break; }
			self->Complete(plan, self->_store->ExecuteLocal(plan)
				? SyncAutoOutcome::Committed : SyncAutoOutcome::NetworkFailure);
			break;
		case SyncAutoAction::Publish:
			self->Complete(plan, self->_store->ExecuteLocal(plan)
				? SyncAutoOutcome::Staged : SyncAutoOutcome::NetworkFailure);
			break;
		case SyncAutoAction::AutoApply: {
			const auto session = self->_session.get();
			const auto replacement = self->_applyReview;
			const auto review = ReviewSyncConfig(session->account(), *session, self->_inventory);
			const auto ok = replacement && replacement->impact.status == ConfigImpactStatus::Ready
				&& !replacement->impact.dangerous && !replacement->usingGood
				&& RebaseAccountTransferAfterSetup(session, *replacement)
				&& self->_store->ExecuteLocal(plan, [&](SyncLocalStore &store) {
				if (!self->Current()) { return false; }
				const auto result = ApplySyncConfigChoice(session->account(), *session,
					self->_inventory, review, plan.apply && plan.apply->source ? std::make_optional(plan.apply->source->head.key) : std::nullopt,
					&*replacement, &store);
				NoteAutomaticSyncApply(session, result);
				if (result.status == SyncConfigApplyStatus::Applied) {
					return true;
				}
				return false;
			});
			self->Complete(plan, ok ? SyncAutoOutcome::Applied : SyncAutoOutcome::NetworkFailure);
		} break;
		case SyncAutoAction::EditInPlace:
		case SyncAutoAction::Post:
			self->Send(plan);
			break;
		case SyncAutoAction::Retire:
			self->Delete(plan);
			break;
		default: self->Stop(); break;
		}
	});
}

void AutoSession::Complete(const SyncAutoPlan &completed, SyncAutoOutcome outcome,
		std::optional<SyncAutoInventory> inventory) {
	if (!Accept(completed)) { return; }
	_inFlight = false;
	auto facts = Facts();
	if (!facts) { Hold(SyncAutoReason::StateChanged); return; }
	if (outcome == SyncAutoOutcome::Inventory && (_scanQueue != SyncConfigSendQueue::Empty
		|| SyncConfigPostsStarted(not_null{ _session.get() }) != _scanPosts)) {
		facts->queue = SyncConfigSendQueue::HoldsSyncRecord;
	}
	const auto plan = _store->Transition(*facts, SyncAutoEvent::Completed,
		int64_t(base::unixtime::now()), completed.ticket, outcome, std::move(inventory));
	Run(plan);
}

void AutoSession::Check(const SyncAutoPlan &plan) {
	const auto lease = SyncChatIoForPost(_session.get(), PeerToken{});
	if (!lease || !lease->current || !lease->current()) {
		Hold(SyncAutoReason::SyncChatNotResolved);
		return;
	}
	const auto weak = base::make_weak(this);
	_scanPosts = SyncConfigPostsStarted(not_null{ _session.get() });
	_scanQueue = SyncConfigSendQueueOf(not_null{ _session.get() });
	_scan = std::make_unique<SyncAccountInventory>(not_null{ _session.get() }, lease->route, nullptr,
		[weak, plan, origin = lease->route.peer.origin](DesktopSyncChatInventory inventory) {
			const auto self = weak.get();
			if (!self || !self->Accept(plan)) { return; }
			if (!DesktopSyncChatInventoryMatches(inventory, origin, PeerToken{})) {
				self->Hold(SyncAutoReason::StateChanged);
				return;
			}
			self->_inventory = std::move(inventory.inventory);
			if (self->_inventory.status == SyncAccountInventoryStatus::Incomplete) {
				self->Complete(plan, SyncAutoOutcome::NetworkFailure); return;
			}
			self->_snapshot = {};
			self->_snapshot.account = self->_account;
			self->_snapshot.chat = self->_store->local().state()->chat;
			self->_snapshot.session = self->_sessionId;
			self->_snapshot.ticket = plan.ticket;
			self->_snapshot.inventory = self->_inventory;
			auto ids = std::set<int32_t>();
			for (const auto &entry : self->_store->local().state()->ownMessages) { ids.insert(entry.messageId); }
			if (self->_inventory.read) {
				for (const auto &entry : self->_inventory.read->records) {
					if (entry.header && entry.header->writerInstall == self->_store->local().state()->install) { ids.insert(entry.id); }
				}
			}
			if (self->_store->record().retiring && self->_store->record().messageId > 0) { ids.insert(self->_store->record().messageId); }
			if (ids.size() > 1024) { self->Complete(plan, SyncAutoOutcome::NetworkFailure); return; }
			self->_readIds.assign(ids.begin(), ids.end());
			self->_readIndex = 0;
			self->ReadNext();
		}, [weak, plan, proof = lease->current] {
			const auto self = weak.get();
			return proof() && self && self->_store && self->Current()
				&& self->_store->CheckCompletion(plan, self->_sessionId) == DesktopSyncAutoCompletion::Ready;
		});
	_scan->Start();
}

void AutoSession::ReadNext() {
	if (!Accept(_plan)) { return; }
	if (_readIndex == _readIds.size()) {
		_snapshot.readAt = int64_t(base::unixtime::now());
		_snapshot.fresh = true;
		Complete(_plan, SyncAutoOutcome::Inventory, _snapshot);
		return;
	}
	const auto lease = SyncChatIoForPost(_session.get(), PeerToken{});
	if (!lease || !lease->current || !lease->current()) {
		Hold(SyncAutoReason::SyncChatNotResolved);
		return;
	}
	const auto weak = base::make_weak(this);
	const auto plan = _plan;
	_exact = std::make_unique<DesktopSyncExactRead>(not_null{ _session.get() }, lease->route, _readIds[_readIndex],
		[weak, plan, proof = lease->current] { const auto self = weak.get(); return proof() && self && self->_store
			&& self->_store->CheckCompletion(plan, self->_sessionId) == DesktopSyncAutoCompletion::Ready; },
		[weak, plan](SyncAutoServerRead read) {
			const auto self = weak.get();
			if (!self || !self->Accept(plan)) { return; }
			if (!read.fresh) { self->Complete(plan, SyncAutoOutcome::NetworkFailure); return; }
			self->_snapshot.reads.push_back(std::move(read));
			++self->_readIndex;
			crl::on_main([weak, ticket = plan.ticket, owner = plan.record.session] {
				if (const auto self = weak.get(); self && self->_sessionId == owner && self->_plan.ticket == ticket) { self->ReadNext(); }
			});
		});
	_exact->Start();
}

void AutoSession::ReadForEffect(const SyncAutoPlan &plan, Fn<void(SyncAutoServerRead)> done) {
	const auto lease = SyncChatIoForPost(_session.get(), PeerToken{});
	if (!lease || !lease->current || !lease->current()) {
		Hold(SyncAutoReason::SyncChatNotResolved);
		return;
	}
	const auto weak = base::make_weak(this);
	_exact = std::make_unique<DesktopSyncExactRead>(not_null{ _session.get() }, lease->route, plan.messageId,
		[weak, plan, proof = lease->current] { const auto self = weak.get(); return proof() && self && self->_store
			&& self->_store->CheckCompletion(plan, self->_sessionId) == DesktopSyncAutoCompletion::Ready
			&& self->_store->Authorized(plan) && SyncChatSelfResolved(self->_session.get()); },
		[weak, plan, done = std::move(done)](SyncAutoServerRead read) mutable {
			const auto self = weak.get();
			if (!self || !self->Accept(plan)) { return; }
			if (!self->_store->Authorized(plan)) { self->Hold(SyncAutoReason::StateChanged); return; }
			done(std::move(read));
		});
	_exact->Start();
}

bool AutoSession::EffectCurrent(const SyncAutoPlan &plan) const {
	return _store && _session && Current() && _store->local().state()
		&& _store->local().state()->version == 1
		&& _store->local().state()->chat.kind == PeerTokenKind::Self
		&& _store->CheckCompletion(plan, _sessionId) == DesktopSyncAutoCompletion::Ready
		&& _store->Authorized(plan);
}

DesktopSyncChatOperation AutoSession::OperationFor(const SyncAutoPlan &plan) {
	if (!_store || _plan.ticket != plan.ticket) {
		return {};
	}
	const auto weak = base::make_weak(this);
	const auto store = _store.get();
	const auto owner = _sessionId;
	return { _store->local().directory(), _account, [weak, store, owner] {
		const auto self = weak.get();
		return self && self->_store.get() == store && self->_sessionId == owner
			&& self->Current() && store->Current() && store->local().state()
			&& store->local().state()->version == 1
			&& store->local().state()->chat.kind == PeerTokenKind::Self;
	} };
}

void AutoSession::RenewEffect(const SyncAutoPlan &plan, Fn<void(bool)> ready) {
	const auto weak = base::make_weak(this);
	CheckSyncChatBeforePost(_session.get(), [weak, plan, ready = std::move(ready)](DesktopSyncChatStatus status) {
		const auto self = weak.get();
		if (!self || !self->EffectCurrent(plan)) { ready(false); return; }
		if (!status.complete || status.postingHeld || status.phase != DesktopSyncChatPhase::Use
			|| status.resolution.readChat != PeerToken{}) { ready(false); return; }
		self->ReadForEffect(plan, [weak, plan, ready](SyncAutoServerRead read) {
			const auto self = weak.get();
			ready(self && self->EffectCurrent(plan) && self->_store->AuthorizeEffect(plan, &read));
		});
	}, OperationFor(plan));
}

void AutoSession::Send(const SyncAutoPlan &plan) {
	const auto weak = base::make_weak(this);
	if (plan.action == SyncAutoAction::Post) {
		if (!_store->AuthorizeEffect(plan)) { Complete(plan, SyncAutoOutcome::OutcomeUnknown); return; }
		_post = std::make_unique<SyncConfigPost>(not_null{ _session.get() }, plan.bytes,
			[weak, plan](SyncConfigPostResult result) {
				if (const auto self = weak.get()) { self->Complete(plan,
					result.status == SyncConfigPostStatus::Confirmed ? SyncAutoOutcome::Sent : SyncAutoOutcome::OutcomeUnknown); }
			}, [weak, plan] {
				const auto self = weak.get();
				return self && self->EffectCurrent(plan);
			}, OperationFor(plan));
		_post->Start();
		return;
	}
	ReadForEffect(plan, [weak, plan](SyncAutoServerRead read) {
		const auto self = weak.get();
		if (!self || !self->_store) { return; }
		const auto authorized = self->_store->AuthorizeEffect(plan, &read);
		if (!authorized) { self->Complete(plan, SyncAutoOutcome::OutcomeUnknown); return; }
		const auto finish = BeginSyncConfigNetworkOperation(not_null{ self->_session.get() });
		self->_upload = std::make_shared<SyncConfigUpload>(*self->_session.get(), plan.bytes,
			[weak, plan, finish](std::optional<MsgId> id) {
				finish();
				if (const auto self = weak.get()) { self->Complete(plan, id ? SyncAutoOutcome::Sent : SyncAutoOutcome::OutcomeUnknown); }
			}, [weak, plan] {
				const auto self = weak.get();
				return self && self->EffectCurrent(plan);
			},
			MsgId(plan.messageId), [weak, plan, finish] {
				finish();
				if (const auto self = weak.get()) { self->Complete(plan, SyncAutoOutcome::EditRefused); }
			}, self->OperationFor(plan), [weak, plan](Fn<void(bool)> ready) {
			const auto self = weak.get();
			if (!self || !self->EffectCurrent(plan)) { ready(false); return; }
			self->ReadForEffect(plan, [weak, plan, ready](SyncAutoServerRead read) {
				const auto self = weak.get();
				ready(self && self->EffectCurrent(plan) && self->_store->AuthorizeEffect(plan, &read));
			});
		});
		self->_upload->Start();
	});
}

void AutoSession::Delete(const SyncAutoPlan &plan) {
	const auto weak = base::make_weak(this);
	ReadForEffect(plan, [weak, plan](SyncAutoServerRead read) {
		const auto self = weak.get();
		if (!self || !self->_store) { return; }
		if (!self->_store->AuthorizeEffect(plan, &read)) {
			self->Complete(plan, SyncAutoOutcome::OutcomeUnknown); return;
		}
		self->_api = std::make_unique<SyncRequestSender>(&self->_session.get()->mtp(), [weak, plan] {
			const auto live = weak.get();
			return live && live->EffectCurrent(plan);
		});
		self->_api->request(MTPmessages_DeleteMessages(MTP_flags(0), MTP_vector<MTPint>(1, MTP_int(plan.messageId))))
		.done([weak, plan](const MTPmessages_AffectedMessages &result) {
			const auto self = weak.get();
			if (!self || !self->Accept(plan)) { return; }
			if (!self->_store->Authorized(plan)) { self->Hold(SyncAutoReason::StateChanged); return; }
			self->_session.get()->api().applyAffectedMessages(self->_session.get()->user(), result);
			self->ReadForEffect(plan, [weak, plan](SyncAutoServerRead absence) {
				const auto self = weak.get();
				if (!self || !self->_store) { return; }
				const auto committed = self->_store->CommitRetirement(plan, absence);
				self->Complete(plan, committed ? SyncAutoOutcome::Deleted : SyncAutoOutcome::OutcomeUnknown);
			});
		}).resumeEffect([weak, plan](Fn<void(bool)> ready) {
			const auto self = weak.get();
			if (!self || !self->EffectCurrent(plan)) { ready(false); return; }
			self->RenewEffect(plan, std::move(ready));
		}).fail([weak, plan](const MTP::Error &error) {
			if (const auto self = weak.get()) { self->Complete(plan,
				error.type() == u"MESSAGE_DELETE_FORBIDDEN"_q ? SyncAutoOutcome::DeletionRefused : SyncAutoOutcome::OutcomeUnknown); }
		}).handleAllErrors().send();
	});
}

}

void StartAutomaticSettingsSync() {
	static auto started = false;
	if (std::exchange(started, true)) { return; }
	static auto lifetime = rpl::lifetime();
	static auto accountsLifetime = rpl::lifetime();
	const auto refresh = [] {
		accountsLifetime.destroy();
		for (const auto &entry : Core::App().domain().accounts()) {
			const auto ready = [](Main::Session *session) {
				if (session) { crl::on_main(session, [=] { For(session).Event(SyncAutoEvent::SessionReady); }); }
			};
			entry.account->sessionChanges() | rpl::on_next(ready, accountsLifetime);
			ready(entry.account->maybeSession());
		}
	};
	SyncRequestChanges() | rpl::on_next([](MTP::Instance *) {
		Changes.fire({});
	}, lifetime);
	SyncChatStatusChanges() | rpl::on_next([](Main::Session *session) {
		Changes.fire({});
		if (session && SyncChatSelfResolved(session)) {
			For(session).Event(SyncAutoEvent::SessionReady);
		}
	}, lifetime);
	Core::App().domain().accountsChanges() | rpl::on_next(refresh, lifetime);
	Core::App().appDeactivatedValue() | rpl::on_next([](bool deactivated) {
		if (!deactivated) { TriggerAutomaticSettingsSync(SyncAutoEvent::Foreground); }
	}, lifetime);
	refresh();
}

void TriggerAutomaticSettingsSync(SyncAutoEvent event) {
	for (const auto &entry : Core::App().domain().accounts()) {
		if (const auto session = entry.account->maybeSession()) { For(session).Event(event); }
	}
}

void SuppressAutomaticSettingsSave(not_null<Main::Session*> session) {
	const auto i = Sessions.find(session.get());
	if (i != Sessions.end()) { i->second->Replaced(); }
}

void ObserveAutomaticSettingsSave(not_null<Main::Session*> session) { For(session).Saved(); }

bool SetAutomaticSettingsSync(not_null<Main::Session*> session, bool enabled) {
	return For(session).Event(enabled ? SyncAutoEvent::Enable : SyncAutoEvent::Disable)
		&& AutomaticSettingsSyncEnabled(session) == enabled;
}

bool AutomaticSettingsSyncEnabled(not_null<Main::Session*> session) {
	const auto target = AccountSyncInspectionTarget(session);
	const auto read = target ? ReadDesktopSyncAuto(target->files, SessionAccountKey(session)) : DesktopSyncAutoRead();
	return read.record && read.record->enabled;
}

QString AutomaticSettingsSyncStatus(not_null<Main::Session*> session) {
	if (!SettingsStartupOwner()) { return tr::lng_purple_auto_owner(tr::now); }
	const auto target = AccountSyncInspectionTarget(session);
	const auto read = target ? ReadDesktopSyncAuto(target->files, SessionAccountKey(session)) : DesktopSyncAutoRead();
	if (!read.record) { return tr::lng_purple_auto_invalid(tr::now); }
	const auto &record = *read.record;
	if (!record.enabled) { return tr::lng_purple_auto_off(tr::now); }
	if (const auto pending = PendingSyncRequest(&session->mtp())) {
		return SyncRequestWaitText(*pending);
	}
	if (!SyncChatSelfResolved(session)) { return tr::lng_purple_auto_chat(tr::now); }
	const auto executor = Sessions.find(session.get());
	const auto failure = executor != Sessions.end() && executor->second->FailureReason() != SyncAutoReason::None
		? executor->second->FailureReason() : record.held;
	if (failure == SyncAutoReason::StateChanged) { return tr::lng_purple_auto_local_failure(tr::now); }
	if (failure == SyncAutoReason::OwnershipChanged) { return tr::lng_purple_auto_context_changed(tr::now); }
	const auto snapshot = ReadSyncRecoveryStore(target->files, SessionAccountKey(session));
	const auto state = snapshot.status == SyncStoreStatus::Ready && snapshot.store.statePresent
		? ParseSyncLocalState(snapshot.store.state) : SyncLocalParseResult();
	if (!state || CheckAccountSyncBinding(state.state, session->account()) != SyncAccountBindingVerdict::Bound) {
		return tr::lng_purple_auto_review(tr::now);
	}
	if (state.state.version != 1 || state.state.chat.kind != PeerTokenKind::Self) { return tr::lng_purple_auto_chat(tr::now); }
	const auto recovery = InspectSyncAccountRecovery(*session);
	if (!recovery || recovery->journalPresent || recovery->plan.status != SyncAccountRecoveryStatus::Bound) {
		return tr::lng_purple_auto_review(tr::now);
	}
	if (record.held == SyncAutoReason::SyncChatNotResolved) { return tr::lng_purple_auto_chat(tr::now); }
	if (record.held == SyncAutoReason::OwnershipUnanswered) { return tr::lng_purple_auto_owner(tr::now); }
	if (record.held != SyncAutoReason::None && record.held != SyncAutoReason::RetryStopped) { return tr::lng_purple_auto_review(tr::now); }
	if (record.runStopped) { return tr::lng_purple_auto_stopped(tr::now); }
	if (record.retryAt) { return tr::lng_purple_auto_retry(tr::now); }
	if (record.active) {
		const auto i = Sessions.find(session.get());
		return i != Sessions.end() && i->second->Running(record)
			? tr::lng_purple_auto_running(tr::now) : tr::lng_purple_auto_review(tr::now);
	}
	return tr::lng_purple_auto_ready(tr::now);
}

rpl::producer<> AutomaticSettingsSyncChanges() { return Changes.events(); }

}

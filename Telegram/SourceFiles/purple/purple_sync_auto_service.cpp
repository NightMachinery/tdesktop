#include "purple/purple_sync_auto_service.h"

#include "purple/purple_sync_chat_service.h"
#include "purple/purple_sync_chat_carry_gate.h"

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
#include "purple/purple_sync_config_publish.h"
#include "purple/purple_sync_config_review.h"
#include "purple/purple_sync_config_text.h"
#include "purple/purple_sync_config_upload.h"
#include "purple/purple_sync_diag.h"
#include "purple/purple_sync_magic_service.h"
#include "purple/purple_sync_request_sender.h"
#include "purple/purple_sync_setup_box.h"

#include <QtCore/QDateTime>
#include <QtCore/QFileInfo>
#include <QtCore/QUuid>

#include <algorithm>
#include <map>
#include <set>

namespace Purple {
namespace {

constexpr auto kSaveDelay = crl::time(5000);
rpl::event_stream<> Changes;

[[nodiscard]] QString FirstUseFailureText(const QString &reason) {
	return (reason == u"busy"_q) ? tr::lng_purple_sync_first_failed_busy(tr::now)
		: (reason == u"unbound"_q) ? tr::lng_purple_sync_first_failed_unbound(tr::now)
		: (reason == u"chat_unavailable"_q) ? tr::lng_purple_sync_first_failed_chat(tr::now)
		: (reason == u"incomplete"_q) ? tr::lng_purple_sync_first_failed_incomplete(tr::now)
		: (reason == u"account_changed"_q) ? tr::lng_purple_sync_first_failed_account(tr::now)
		: (reason == u"recovery_journal"_q) ? tr::lng_purple_sync_first_failed_journal(tr::now)
		: (reason == u"files_unwritable"_q || reason == u"unreadable"_q) ? tr::lng_purple_sync_first_failed_files(tr::now)
		: (reason == u"archive_refused"_q) ? tr::lng_purple_sync_first_failed_archive(tr::now)
		: (reason == u"settings_invalid"_q) ? tr::lng_purple_sync_first_wait_file(tr::now)
		: (reason == u"chat_changed"_q) ? tr::lng_purple_sync_first_failed_changed(tr::now)
		: (reason == u"publish_unconfirmed"_q) ? tr::lng_purple_sync_first_failed_unconfirmed(tr::now)
		: (reason == u"publish_refused"_q) ? tr::lng_purple_sync_first_failed_publish(tr::now)
		: tr::lng_purple_sync_first_failed(tr::now);
}

[[nodiscard]] QString FirstUsePublishReason(SyncConfigPublishStatus status) {
	switch (status) {
	case SyncConfigPublishStatus::NeedsReview:
	case SyncConfigPublishStatus::CloneDetected:
	case SyncConfigPublishStatus::Incomplete: return u"chat_changed"_q;
	case SyncConfigPublishStatus::AccountUnavailable:
	case SyncConfigPublishStatus::AccountUnbound: return u"account_changed"_q;
	case SyncConfigPublishStatus::StoreError: return u"files_unwritable"_q;
	case SyncConfigPublishStatus::InvalidSettings: return u"settings_invalid"_q;
	case SyncConfigPublishStatus::OutcomeUnknown:
	case SyncConfigPublishStatus::StillSending: return u"publish_unconfirmed"_q;
	case SyncConfigPublishStatus::Confirmed:
	case SyncConfigPublishStatus::AlreadySynced:
	case SyncConfigPublishStatus::Cancelled: break;
	}
	return u"publish_refused"_q;
}

class AutoSession final : public base::has_weak_ptr {
public:
	explicit AutoSession(not_null<Main::Session*> session);
	~AutoSession();
	bool Event(SyncAutoEvent event);
	void ChatChanged();
	SyncAutoReason FailureReason() const { return _failureReason; }
	void Saved();
	void Replaced();
	bool MoveToBound();
	bool Running(const SyncAutoRecord &record) const {
		return _inFlight && _store && record.active && record.session == _sessionId && record.ticket == _plan.ticket;
	}
	void FirstUse(std::optional<DesktopSyncChatInventory> inventory, SyncFirstUseChoice choice,
		Fn<void(SyncFirstUseResult)> done, bool manual);
	[[nodiscard]] std::optional<SyncFirstUseResult> FirstUseResult() const {
		return _firstRunning ? std::nullopt : _firstUse;
	}
	[[nodiscard]] bool FirstUseRunning() const { return _firstRunning; }

private:
	bool Current() const;
	bool Open();
	void MoveStore();
	[[nodiscard]] PeerToken StateChat() const;
	[[nodiscard]] bool Resolved() const;
	std::optional<SyncAutoFacts> Facts();
	bool Accept(const SyncAutoPlan &plan);
	void Hold(SyncAutoReason reason);
	void Run(SyncAutoPlan plan);
	void Complete(const SyncAutoPlan &plan, SyncAutoOutcome outcome,
		std::optional<SyncAutoInventory> inventory = std::nullopt);
	void Check(const SyncAutoPlan &plan);
	bool PauseOnForeignSpace(PeerToken chat);
	bool NoStateEvent(SyncAutoEvent event);
	[[nodiscard]] bool FirstUseDue(const AccountFiles &files, SyncAutoEvent event);
	void RetireCarry();
	void FirstUseRun(DesktopSyncChatInventory inventory, SyncFirstUseChoice choice, bool manual);
	void FirstUseJoined(const DesktopSyncChatInventory &inventory, const SyncFirstUseReview &review);
	void FirstUseFinish(SyncFirstUseOutcome outcome, QString text = QString(), QString reason = QString());
	void FirstUseFail(const QString &reason, QString text = QString());
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
	rpl::lifetime _lifetime;
	QString _savedFingerprint;
	QString _pendingFingerprint;
	std::optional<AccountSettingsOpenTarget> _pendingOpening;
	QByteArray _pendingIdentity;
	QByteArray _savedIdentity;
	std::unique_ptr<SyncAccountInventory> _firstScan;
	std::unique_ptr<SyncConfigPublish> _firstPublish;
	std::vector<Fn<void(SyncFirstUseResult)>> _firstDone;
	std::optional<SyncFirstUseResult> _firstUse;
	QString _firstFingerprint;
	bool _firstRunning = false;
	bool _firstBypass = false;

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
	SyncChatPublisherQuiescence() | rpl::on_next([weak = base::make_weak(this)](Main::Session *session) {
		if (const auto self = weak.get(); self && self->_session.get() == session) { self->Stop(); }
	}, _lifetime);

}

AutoSession::~AutoSession() { Stop(); }

bool AutoSession::Current() const {
	const auto session = _session.get();
	if (!session || DesktopSyncCarryGateFor(_account)->postingHeld()
		|| session->account().loggingOut() || session->account().maybeSession() != session
		|| SessionAccountKey(session) != _account || !_opening
		|| AccountSyncBindingToken(session->account()) != _token) { return false; }
	const auto opening = AccountSettingsOpenTargetFor(session, SettingsOpenRole::Own);
	return opening && SameDesktopSyncAutoContext(*_opening, *opening);
}

bool AutoSession::Open() {
	if (_store) { return Current() && _store->Current(); }
	const auto session = _session.get();
	if (!session || DesktopSyncCarryGateFor(_account)->postingHeld()
		|| session->account().loggingOut() || session->account().maybeSession() != session) { return false; }
	_opening = AccountSettingsOpenTargetFor(session, SettingsOpenRole::Own);
	const auto target = AccountSyncInspectionTarget(session);
	if (!_opening || !target || !SettingsStartupOwner()) { return false; }
	RetireCarry();
	const auto recovery = InspectSyncAccountRecovery(*session);
	if (!recovery || recovery->journalPresent || recovery->plan.status != SyncAccountRecoveryStatus::Bound) { return false; }
	_token = AccountSyncBindingToken(session->account());
	const auto weak = base::make_weak(this);
	_store = std::make_unique<DesktopSyncAutoStore>(target->files, _account, _token, [weak] {
		const auto self = weak.get();
		return self && self->Current();
	});
	if (!_store->Open()) { _store.reset(); return false; }
	MoveStore();
	return true;
}

void AutoSession::MoveStore() {
	const auto session = _session.get();
	const auto bound = session ? SyncMagicBound(session) : std::nullopt;
	if (!_store || !bound || *bound == StateChat()) { return; }
	const auto local = ReadSyncSettingsFile(_store->files());
	const auto moved = !local.fingerprint.isEmpty()
		&& _store->MoveTo(*bound, session->userId().bare, local.fingerprint);
	SyncDiag(u"sync_chat"_q, u"migration"_q, {
		SyncDiagCode(u"step"_q, u"move"_q),
		SyncDiagCode(u"outcome"_q, moved ? u"ok"_q : u"error"_q) });
}

// WHY: manual Check and Share need the state in the bound chat too, so the move runs whether or not
// automatic sync is on; a store opened only for it is closed again, releasing the account lock.
bool AutoSession::MoveToBound() {
	const auto session = _session.get();
	const auto bound = session ? SyncMagicBound(session) : std::nullopt;
	if (!bound || _inFlight) { return false; }
	const auto opened = (_store != nullptr);
	if (!Open()) { return false; }
	if (opened) { MoveStore(); }
	const auto moved = (StateChat() == *bound);
	if (!opened) {
		_store.reset();
		_opening.reset();
	}
	return moved;
}

PeerToken AutoSession::StateChat() const {
	const auto state = _store ? _store->local().state() : nullptr;
	return state ? state->chat : PeerToken{};
}

bool AutoSession::Resolved() const {
	const auto session = _session.get();
	const auto bound = session ? SyncMagicBound(session) : std::nullopt;
	return _store && _store->local().state() && bound && *bound == StateChat();
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
	facts.chatResolved = Resolved();
	facts.chatProblem = SyncMagicPaused(not_null{ _session.get() });
	facts.chatWritable = true;
	facts.local = ReadSyncSettingsFile(_store->files());
	facts.savedFingerprint = SyncPathIdentity(_store->files().settings) == _savedIdentity
		? _savedFingerprint : QString();
	facts.saveDebounceFinished = !_save.isActive();
	facts.queue = SyncConfigSendQueueOf(not_null{ _session.get() }, facts.chat);
	if (_inFlight && (_scanQueue != SyncConfigSendQueue::Empty
		|| SyncConfigPostsStarted(not_null{ _session.get() }, facts.chat) != _scanPosts)) {
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
	if (event != SyncAutoEvent::Disable && !SyncMagicBound(session) && !SyncMagicPaused(session)) {
		Stop();
		SyncMagicLookup(session, SyncMagicTrigger::Auto, false);
		return false;
	}
	if (!Open()) { Stop(); return NoStateEvent(event); }
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

void AutoSession::ChatChanged() {
	_firstBypass = true;
	Event(SyncAutoEvent::SessionReady);
	_firstBypass = false;
}

bool AutoSession::Accept(const SyncAutoPlan &plan) {
	if (!_inFlight || !_store || _plan.ticket != plan.ticket || _sessionId != plan.record.session) { return false; }
	const auto completion = _store->CheckCompletion(plan, _sessionId);
	if (completion == DesktopSyncAutoCompletion::Obsolete) { return false; }
	if (completion != DesktopSyncAutoCompletion::Ready) {
		Hold(completion == DesktopSyncAutoCompletion::ContextChanged
			? SyncAutoReason::OwnershipChanged : SyncAutoReason::StateChanged);
		return false;
	}
	if (!Resolved()) {
		Hold(SyncAutoReason::SyncChatNotResolved);
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
		const auto session = _session.get();
		if (!retry && session && plan.reason == SyncAutoReason::NeedsReview && _inventory.read) {
			NoteSyncConfigRefusal(ReviewSyncConfig(session->account(), *session, _inventory, StateChat()), false);
		}
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
			const auto review = ReviewSyncConfig(session->account(), *session, self->_inventory, self->StateChat());
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
		|| SyncConfigPostsStarted(not_null{ _session.get() }, StateChat()) != _scanPosts)) {
		facts->queue = SyncConfigSendQueue::HoldsSyncRecord;
	}
	const auto plan = _store->Transition(*facts, SyncAutoEvent::Completed,
		int64_t(base::unixtime::now()), completed.ticket, outcome, std::move(inventory));
	Run(plan);
}

void AutoSession::Check(const SyncAutoPlan &plan) {
	const auto chat = StateChat();
	const auto lease = SyncChatIoForPost(_session.get(), chat);
	if (!lease || !lease->current || !lease->current()) {
		Hold(SyncAutoReason::SyncChatNotResolved);
		return;
	}
	const auto weak = base::make_weak(this);
	_scanPosts = SyncConfigPostsStarted(not_null{ _session.get() }, chat);
	_scanQueue = SyncConfigSendQueueOf(not_null{ _session.get() }, chat);
	_scan = std::make_unique<SyncAccountInventory>(not_null{ _session.get() }, lease->route, nullptr,
		[weak, plan, chat, origin = lease->route.peer.origin](DesktopSyncChatInventory inventory) {
			const auto self = weak.get();
			if (!self || !self->Accept(plan)) { return; }
			if (!DesktopSyncChatInventoryMatches(inventory, origin, chat)) {
				self->Hold(SyncAutoReason::StateChanged);
				return;
			}
			self->_inventory = std::move(inventory.inventory);
			if (self->_inventory.status == SyncAccountInventoryStatus::Incomplete) {
				self->Complete(plan, SyncAutoOutcome::NetworkFailure); return;
			}
			if (self->PauseOnForeignSpace(chat)) { return; }
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

bool AutoSession::PauseOnForeignSpace(PeerToken chat) {
	const auto magic = SyncMagicChatOf(not_null{ _session.get() });
	// WHY: the chat may also hold this state's own space when another device won the race to start it;
	// any state outside the selected space is first use, as the Sync screen's Check decides it.
	const auto first = (magic && chat != PeerToken{} && _store && _store->local().state())
		? ReviewSyncFirstUse(_store->files(), _inventory) : SyncFirstUseReview();
	if (first.settled) {
		ClearSyncFirstUseMark(_store->files());
	}
	if ((first.need != SyncFirstUseNeed::OtherSpace && first.need != SyncFirstUseNeed::Unjoined) || !first.action) {
		_firstUse.reset();
		return false;
	}
	const auto lease = SyncChatIoForPost(_session.get(), chat);
	if (!lease) {
		Hold(SyncAutoReason::SyncChatNotResolved);
		return true;
	}
	auto inventory = DesktopSyncChatInventory{ lease->route, _inventory };
	Stop();
	crl::on_main([weak = base::make_weak(this), inventory = std::move(inventory)]() mutable {
		if (const auto self = weak.get()) {
			self->FirstUse(std::move(inventory), SyncFirstUseChoice::None, nullptr, false);
		}
	});
	return true;
}

void AutoSession::RetireCarry() {
	const auto session = _session.get();
	const auto target = session ? AccountSyncInspectionTarget(session) : std::nullopt;
	if (!target || !SyncMagicBound(session) || DesktopSyncCarryGateFor(_account)->postingHeld()
		|| !SyncCarryLeftoversPresent(target->files.syncDirectory)) { return; }
	const auto date = QDateTime::currentDateTimeUtc().toString(u"yyyyMMdd-HHmmss"_q);
	const auto retired = RetireSyncCarryLeftovers(target->files.syncDirectory, date);
	SyncDiag(u"sync_chat"_q, u"migration"_q, {
		SyncDiagCode(u"step"_q, u"carry"_q),
		SyncDiagCode(u"outcome"_q, retired ? u"ok"_q : u"error"_q) });
}

bool AutoSession::NoStateEvent(SyncAutoEvent event) {
	const auto session = _session.get();
	const auto target = session ? AccountSyncInspectionTarget(session) : std::nullopt;
	if (!target || !SyncMagicBound(session)) { return false; }
	RetireCarry();
	const auto recovery = InspectSyncAccountRecovery(*session);
	if (!recovery || recovery->journalPresent || recovery->plan.status != SyncAccountRecoveryStatus::Setup) {
		return false;
	} else if ((event == SyncAutoEvent::Enable || event == SyncAutoEvent::Disable)
		&& !SetDesktopSyncAutoBeforeState(target->files, _account, event == SyncAutoEvent::Enable)) {
		return false;
	} else if (event != SyncAutoEvent::Disable && FirstUseDue(target->files, event)) {
		FirstUse(std::nullopt, SyncFirstUseChoice::None, nullptr, false);
	}
	Changes.fire({});
	return true;
}

// WHY: without a state the store's lifecycle limit never runs, so the last first-use outcome beside the
// missing state limits the scans; Enable, a sync chat change and a changed settled file still decide afresh.
bool AutoSession::FirstUseDue(const AccountFiles &files, SyncAutoEvent event) {
	if (event == SyncAutoEvent::Enable || _firstBypass) {
		return true;
	} else if (event == SyncAutoEvent::LocalSaved && _save.isActive()) {
		return false;
	}
	const auto mark = ReadSyncFirstUseMark(files);
	const auto now = int64_t(base::unixtime::now());
	return !mark || mark->checkedAt <= 0 || now < mark->checkedAt
		|| now - mark->checkedAt >= kSyncAutoCheckSeconds
		|| mark->fingerprint != ReadSyncSettingsFile(files).fingerprint;
}

void AutoSession::FirstUse(std::optional<DesktopSyncChatInventory> inventory, SyncFirstUseChoice choice,
		Fn<void(SyncFirstUseResult)> done, bool manual) {
	if (done) { _firstDone.push_back(std::move(done)); }
	if (_firstRunning) { return; }
	const auto session = _session.get();
	const auto bound = session ? SyncMagicBound(session) : std::nullopt;
	const auto files = session ? AccountSyncInspectionTarget(session) : std::nullopt;
	_firstFingerprint = files ? ReadSyncSettingsFile(files->files).fingerprint : QString();
	if (!session || !bound || _inFlight) { FirstUseFail(_inFlight ? u"busy"_q : u"unbound"_q); return; }
	_firstRunning = true;
	Changes.fire({});
	if (inventory) {
		FirstUseRun(std::move(*inventory), choice, manual);
		return;
	}
	const auto lease = SyncChatIoForPost(session, *bound);
	if (!lease || !lease->current || !lease->current()) { FirstUseFail(u"chat_unavailable"_q); return; }
	const auto weak = base::make_weak(this);
	_firstScan = std::make_unique<SyncAccountInventory>(not_null{ session }, lease->route, nullptr,
		[weak, choice, manual](DesktopSyncChatInventory result) {
			const auto self = weak.get();
			if (!self || !self->_firstRunning) { return; }
			crl::on_main([weak, choice, manual, result = std::move(result)]() mutable {
				if (const auto self = weak.get(); self && self->_firstRunning) {
					self->_firstScan.reset();
					self->FirstUseRun(std::move(result), choice, manual);
				}
			});
		}, lease->current);
	_firstScan->Start();
}

void AutoSession::FirstUseRun(DesktopSyncChatInventory inventory, SyncFirstUseChoice choice, bool manual) {
	const auto session = _session.get();
	const auto bound = session ? SyncMagicBound(session) : std::nullopt;
	const auto target = session ? AccountSyncInspectionTarget(session) : std::nullopt;
	if (!session || !bound || *bound != inventory.route.peer.chat || !target) {
		FirstUseFail(u"account_changed"_q);
		return;
	} else if (inventory.inventory.status != SyncAccountInventoryStatus::Complete) {
		FirstUseFail(u"incomplete"_q);
		return;
	}
	RetireCarry();
	const auto recovery = InspectSyncAccountRecovery(*session, &inventory.inventory);
	if (!recovery || recovery->journalPresent) {
		FirstUseFail(recovery ? u"recovery_journal"_q : u"unreadable"_q);
		return;
	}
	const auto review = ReviewSyncFirstUse(target->files, inventory.inventory, choice);
	if (review.need == SyncFirstUseNeed::None) { FirstUseFinish(SyncFirstUseOutcome::NotNeeded); return; }
	if (!review.action) {
		const auto refused = ReviewSyncConfig(session->account(), *session, inventory.inventory, *bound);
		NoteSyncConfigRefusal(refused, manual);
		const auto code = SyncConfigReviewReasonCode(refused.reason);
		FirstUseFinish(SyncFirstUseOutcome::Failed, DescribeSyncConfigReview(refused, QDateTime::currentDateTime()).text,
			code.isEmpty() ? u"unknown"_q : code);
		return;
	}
	const auto action = *review.action;
	SyncDiag(u"sync_chat"_q, u"first_use"_q, {
		SyncDiagCode(u"trigger"_q, manual ? u"manual"_q : u"auto"_q),
		SyncDiagCode(u"action"_q, action == SyncFirstUseAction::Wait ? u"wait"_q
			: action == SyncFirstUseAction::Publish ? u"publish"_q
			: action == SyncFirstUseAction::Join ? u"join"_q
			: action == SyncFirstUseAction::JoinSilently ? u"join_silently"_q : u"ask"_q),
		SyncDiagCode(u"choice"_q, review.input.choice == SyncFirstUseChoice::Synced ? u"synced"_q
			: review.input.choice == SyncFirstUseChoice::ThisDevice ? u"this_device"_q : u"none"_q) });
	const auto broken = (review.input.local.status == SyncSettingsFileStatus::Invalid)
		|| review.input.local.usingLastGood;
	if (action == SyncFirstUseAction::Wait && broken) {
		FirstUseFail(u"settings_invalid"_q);
		return;
	} else if (action == SyncFirstUseAction::Wait || action == SyncFirstUseAction::Ask) {
		FirstUseFinish(action == SyncFirstUseAction::Wait ? SyncFirstUseOutcome::Waiting : SyncFirstUseOutcome::Asking);
		return;
	}
	const auto publish = (action == SyncFirstUseAction::Publish)
		|| (action == SyncFirstUseAction::JoinSilently && !review.sameBytes);
	auto mark = ReadSyncFirstUseMark(target->files).value_or(SyncFirstUseMark());
	mark.choice = review.input.choice;
	mark.heads = review.heads;
	mark.joinSpace = publish ? QString() : inventory.inventory.directory.selectedSpace.value_or(QString());
	if (!WriteSyncFirstUseMark(target->files, mark)) {
		FirstUseFail(u"files_unwritable"_q);
		return;
	} else if (review.need == SyncFirstUseNeed::Unjoined) {
		FirstUseJoined(inventory, review);
		return;
	}
	if (review.need == SyncFirstUseNeed::OtherSpace) {
		Stop();
		const auto fresh = InspectSyncAccountRecovery(*session, &inventory.inventory);
		const auto archived = fresh && !fresh->journalPresent
			&& StartAgainSyncAccount(*session, *fresh, true, false, true).ready;
		SyncDiag(u"sync_chat"_q, u"first_use"_q, {
			SyncDiagCode(u"step"_q, u"archive"_q),
			SyncDiagCode(u"outcome"_q, archived ? u"ok"_q : u"error"_q) });
		if (!archived) { FirstUseFail(u"archive_refused"_q); return; }
	}
	const auto chat = inventory.route.peer.chat;
	const auto weak = base::make_weak(this);
	_firstPublish = std::make_unique<SyncConfigPublish>(session->account(), *session, inventory,
		SyncConfigSendQueueOf(not_null{ session }, chat), SyncConfigPostsStarted(not_null{ session }, chat),
		[weak, publish, inventory, review](SyncConfigPublishResult result) {
			crl::on_main([weak, publish, inventory, review, result] {
				const auto self = weak.get();
				if (!self || !self->_firstRunning) { return; }
				self->_firstPublish.reset();
				SyncDiag(u"sync_chat"_q, u"first_use"_q, {
					SyncDiagCode(u"step"_q, publish ? u"publish"_q : u"join"_q),
					SyncDiagCode(u"outcome"_q, (result.status == SyncConfigPublishStatus::Confirmed
						|| result.status == SyncConfigPublishStatus::AlreadySynced) ? u"ok"_q : u"error"_q) });
				const auto sent = (result.status == SyncConfigPublishStatus::Confirmed
					|| result.status == SyncConfigPublishStatus::AlreadySynced);
				if (!sent || (!publish && result.status != SyncConfigPublishStatus::AlreadySynced)) {
					self->FirstUseFail(FirstUsePublishReason(result.status));
				} else if (publish && review.action == SyncFirstUseAction::JoinSilently) {
					self->FirstUseFinish(SyncFirstUseOutcome::Joined, tr::lng_purple_sync_first_joined_same(tr::now));
				} else if (publish) {
					self->FirstUseFinish(SyncFirstUseOutcome::Published);
				} else {
					self->FirstUseJoined(inventory, review);
				}
			});
		}, publish ? review.request : SyncConfigPublishRequest(), std::nullopt,
		[weak, chat] {
			const auto self = weak.get();
			const auto session = self ? self->_session.get() : nullptr;
			return session && SyncMagicBound(session) == chat;
		}, !publish);
	_firstPublish->Start();
}

void AutoSession::FirstUseJoined(const DesktopSyncChatInventory &inventory, const SyncFirstUseReview &review) {
	const auto session = _session.get();
	const auto target = session ? AccountSyncInspectionTarget(session) : std::nullopt;
	const auto local = target ? ReadSyncSettingsFile(target->files) : SyncSettingsFile();
	const auto current = session ? ReviewAccountTransfer(session,
		local.status == SyncSettingsFileStatus::Present ? local.text : QByteArray("version = 2\n")) : std::nullopt;
	if (!session || !current) { FirstUseFail(u"account_changed"_q); return; }
	const auto applied = ApplySyncFirstUseJoin(session->account(), *session, inventory.inventory,
		review.headKey, *current, inventory.route.peer.chat);
	NoteAutomaticSyncApply(session, applied);
	SyncDiag(u"sync_chat"_q, u"first_use"_q, {
		SyncDiagCode(u"step"_q, u"apply"_q),
		SyncDiagCode(u"outcome"_q, applied.status == SyncConfigApplyStatus::Applied ? u"ok"_q : u"error"_q) });
	if (applied.status != SyncConfigApplyStatus::Applied) {
		FirstUseFail(u"apply_refused"_q, SyncConfigApplyFailureText(applied));
		return;
	}
	FirstUseFinish(SyncFirstUseOutcome::Joined, !applied.wroteFile ? tr::lng_purple_sync_first_joined_same(tr::now)
		: applied.undoAvailable ? tr::lng_purple_sync_first_joined(tr::now)
		: tr::lng_purple_sync_first_joined_new(tr::now));
}

void AutoSession::FirstUseFail(const QString &reason, QString text) {
	SyncDiag(u"sync_chat"_q, u"first_use"_q, {
		SyncDiagCode(u"outcome"_q, u"error"_q),
		SyncDiagReason(reason) });
	FirstUseFinish(SyncFirstUseOutcome::Failed, text.isEmpty() ? FirstUseFailureText(reason) : text, reason);
}

void AutoSession::FirstUseFinish(SyncFirstUseOutcome outcome, QString text, QString reason) {
	_firstRunning = false;
	_firstScan.reset();
	_firstPublish.reset();
	if (text.isEmpty()) {
		text = (outcome == SyncFirstUseOutcome::Waiting) ? tr::lng_purple_sync_first_wait(tr::now)
			: (outcome == SyncFirstUseOutcome::Asking) ? tr::lng_purple_sync_first_ask(tr::now)
			: (outcome == SyncFirstUseOutcome::Published) ? tr::lng_purple_sync_first_published(tr::now)
			: (outcome == SyncFirstUseOutcome::Failed) ? tr::lng_purple_sync_first_failed(tr::now)
			: QString();
	}
	const auto result = SyncFirstUseResult{ outcome, text, reason };
	_firstUse = result;
	const auto session = _session.get();
	const auto target = session ? AccountSyncInspectionTarget(session) : std::nullopt;
	if (target && (outcome == SyncFirstUseOutcome::Published || outcome == SyncFirstUseOutcome::Joined)) {
		ClearSyncFirstUseMark(target->files);
	} else if (target && outcome != SyncFirstUseOutcome::NotNeeded) {
		if (auto mark = ReadSyncFirstUseMark(target->files)) {
			mark->checkedAt = int64_t(base::unixtime::now());
			mark->fingerprint = _firstFingerprint;
			if (!WriteSyncFirstUseMark(target->files, *mark)) {
				SyncDiag(u"sync_chat"_q, u"first_use"_q, {
					SyncDiagCode(u"step"_q, u"mark"_q),
					SyncDiagCode(u"outcome"_q, u"error"_q) });
			}
		}
	}
	Changes.fire({});
	for (const auto &done : base::take(_firstDone)) { done(result); }
	if (session && (outcome == SyncFirstUseOutcome::Published || outcome == SyncFirstUseOutcome::Joined)
		&& AutomaticSettingsSyncEnabled(session)) {
		Event(SyncAutoEvent::SessionReady);
	}
}

void AutoSession::ReadNext() {
	if (!Accept(_plan)) { return; }
	if (_readIndex == _readIds.size()) {
		_snapshot.readAt = int64_t(base::unixtime::now());
		_snapshot.fresh = true;
		Complete(_plan, SyncAutoOutcome::Inventory, _snapshot);
		return;
	}
	const auto lease = SyncChatIoForPost(_session.get(), StateChat());
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
	const auto lease = SyncChatIoForPost(_session.get(), StateChat());
	if (!lease || !lease->current || !lease->current()) {
		Hold(SyncAutoReason::SyncChatNotResolved);
		return;
	}
	const auto weak = base::make_weak(this);
	_exact = std::make_unique<DesktopSyncExactRead>(not_null{ _session.get() }, lease->route, plan.messageId,
		[weak, plan, proof = lease->current] { const auto self = weak.get(); return proof() && self && self->_store
			&& self->_store->CheckCompletion(plan, self->_sessionId) == DesktopSyncAutoCompletion::Ready
			&& self->_store->Authorized(plan) && self->Resolved(); },
		[weak, plan, done = std::move(done)](SyncAutoServerRead read) mutable {
			const auto self = weak.get();
			if (!self || !self->Accept(plan)) { return; }
			if (!self->_store->Authorized(plan)) { self->Hold(SyncAutoReason::StateChanged); return; }
			done(std::move(read));
		});
	_exact->Start();
}

bool AutoSession::EffectCurrent(const SyncAutoPlan &plan) const {
	return _store && _session && Current() && Resolved()
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
			&& self->Current() && store->Current() && self->Resolved();
	} };
}

void AutoSession::RenewEffect(const SyncAutoPlan &plan, Fn<void(bool)> ready) {
	const auto weak = base::make_weak(this);
	CheckSyncChatBeforePost(_session.get(), [weak, plan, ready = std::move(ready)](DesktopSyncChatStatus status) {
		const auto self = weak.get();
		if (!self || !self->EffectCurrent(plan)) { ready(false); return; }
		if (!status.complete || status.postingHeld || status.phase != DesktopSyncChatPhase::Use
			|| status.resolution.readChat != self->StateChat()) { ready(false); return; }
		self->ReadForEffect(plan, [weak, plan, ready](SyncAutoServerRead read) {
			const auto self = weak.get();
			ready(self && self->EffectCurrent(plan) && self->_store->AuthorizeEffect(plan, &read));
		});
	}, OperationFor(plan));
}

void AutoSession::Send(const SyncAutoPlan &plan) {
	const auto weak = base::make_weak(this);
	if (plan.action == SyncAutoAction::Post) {
		const auto lease = SyncChatIoForPost(_session.get(), StateChat());
		if (!lease) { Hold(SyncAutoReason::SyncChatNotResolved); return; }
		if (!_store->AuthorizeEffect(plan)) { Complete(plan, SyncAutoOutcome::OutcomeUnknown); return; }
		_post = std::make_unique<SyncConfigPost>(not_null{ _session.get() }, lease->route, plan.bytes,
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
		const auto chat = self->StateChat();
		const auto lease = SyncChatIoForPost(self->_session.get(), chat);
		if (!lease) { self->Hold(SyncAutoReason::SyncChatNotResolved); return; }
		const auto authorized = self->_store->AuthorizeEffect(plan, &read);
		if (!authorized) { self->Complete(plan, SyncAutoOutcome::OutcomeUnknown); return; }
		const auto finish = BeginSyncConfigNetworkOperation(not_null{ self->_session.get() }, chat);
		self->_upload = std::make_shared<SyncConfigUpload>(*self->_session.get(), lease->route, plan.bytes,
			[weak, plan, finish](std::optional<MsgId> id) {
				finish();
				if (const auto self = weak.get()) { self->Complete(plan, id ? SyncAutoOutcome::Sent : SyncAutoOutcome::OutcomeUnknown); }
			}, [weak, plan] {
				const auto self = weak.get();
				return self && self->EffectCurrent(plan);
			}, self->OperationFor(plan),
			MsgId(plan.messageId), [weak, plan, finish] {
				finish();
				if (const auto self = weak.get()) { self->Complete(plan, SyncAutoOutcome::EditRefused); }
			}, [weak, plan](Fn<void(bool)> ready) {
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
		const auto lease = SyncChatIoForPost(self->_session.get(), self->StateChat());
		const auto channel = lease ? DesktopSyncChatInputChannel(lease->route) : std::nullopt;
		if (!lease || (self->StateChat() != PeerToken{} && !channel)) {
			self->Hold(SyncAutoReason::SyncChatNotResolved);
			return;
		}
		if (!self->_store->AuthorizeEffect(plan, &read)) {
			self->Complete(plan, SyncAutoOutcome::OutcomeUnknown); return;
		}
		self->_api = std::make_unique<SyncRequestSender>(&self->_session.get()->mtp(), [weak, plan] {
			const auto live = weak.get();
			return live && live->EffectCurrent(plan);
		});
		const auto send = [&](auto &&request) {
			request.done([weak, plan](const MTPmessages_AffectedMessages &result) {
				const auto self = weak.get();
				if (!self || !self->Accept(plan)) { return; }
				if (!self->_store->Authorized(plan)) { self->Hold(SyncAutoReason::StateChanged); return; }
				if (const auto peer = SyncMagicBoundPeer(not_null{ self->_session.get() })) {
					self->_session.get()->api().applyAffectedMessages(peer, result);
				}
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
		};
		const auto ids = MTP_vector<MTPint>(1, MTP_int(plan.messageId));
		if (channel) {
			send(self->_api->request(MTPchannels_DeleteMessages(*channel, ids)));
		} else {
			send(self->_api->request(MTPmessages_DeleteMessages(MTP_flags(0), ids)));
		}
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
		if (session && SyncMagicBound(session)) {
			For(session).MoveToBound();
		}
		if (session && (SyncMagicBound(session) || SyncMagicPaused(session))) {
			For(session).ChatChanged();
		}
	}, lifetime);
	Core::App().domain().accountsChanges() | rpl::on_next(refresh, lifetime);
	Core::App().domain().activeSessionChanges() | rpl::on_next([=] { refresh(); }, lifetime);
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

bool MoveSyncStateToBoundChat(not_null<Main::Session*> session) { return For(session).MoveToBound(); }

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
	if (!SyncMagicBound(session)) { return tr::lng_purple_auto_chat(tr::now); }
	const auto executor = Sessions.find(session.get());
	if (executor != Sessions.end() && executor->second->FirstUseRunning()) {
		return tr::lng_purple_auto_running(tr::now);
	} else if (const auto first = SyncFirstUseResultOf(session)) {
		if (first->outcome == SyncFirstUseOutcome::Waiting) { return tr::lng_purple_auto_first_wait(tr::now); }
		if (first->outcome == SyncFirstUseOutcome::Asking) { return tr::lng_purple_auto_first_ask(tr::now); }
		if (first->outcome == SyncFirstUseOutcome::Failed && (first->reason == u"incomplete"_q || first->reason == u"chat_unavailable"_q)) { return tr::lng_purple_auto_retry(tr::now); }
	}
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
	const auto recovery = InspectSyncAccountRecovery(*session);
	if (!recovery || recovery->journalPresent || recovery->plan.status != SyncAccountRecoveryStatus::Bound) {
		return tr::lng_purple_auto_review(tr::now);
	}
	if (record.held == SyncAutoReason::SyncChatNotResolved
		|| record.held == SyncAutoReason::SyncChatProblem) { return tr::lng_purple_auto_chat(tr::now); }
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

void RunSyncFirstUse(
		not_null<Main::Session*> session,
		std::optional<DesktopSyncChatInventory> inventory,
		SyncFirstUseChoice choice,
		Fn<void(SyncFirstUseResult)> done) {
	For(session).FirstUse(std::move(inventory), choice, std::move(done), true);
}

std::optional<SyncFirstUseResult> SyncFirstUseResultOf(not_null<Main::Session*> session) {
	const auto i = Sessions.find(session.get());
	return (i != Sessions.end()) ? i->second->FirstUseResult() : std::nullopt;
}

void NoteSyncConfigRefusal(const SyncConfigReview &review, bool manual) {
	if (review.status != SyncConfigReviewStatus::NeedsReview) { return; }
	const auto code = SyncConfigReviewReasonCode(review.reason);
	SyncDiag(u"sync_chat"_q, u"review_refused"_q, {
		SyncDiagCode(u"trigger"_q, manual ? u"manual"_q : u"auto"_q),
		SyncDiagReason(code.isEmpty() ? u"unknown"_q : code) });
}

}

#include "purple/purple_sync_config_publish_flow.h"

#include "base/random.h"
#include "purple/purple_sync_account_recovery.h"
#include "purple/purple_sync_chat_create.h"
#include "purple/purple_sync_chat_io.h"
#include "purple/purple_sync_chat_store.h"
#include "purple/purple_sync_config_review.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QThread>

namespace Purple {
namespace {

QByteArray RandomBytes(int size) {
	auto result = QByteArray(size, '\0');
	base::RandomFill(result.data(), result.size());
	return result;
}

bool Worker() {
	const auto app = QCoreApplication::instance();
	return !app || QThread::currentThread() != app->thread();
}

}

SyncConfigPublishFlow::SyncConfigPublishFlow(SyncConfigPublishDiskInput input,
		std::shared_ptr<std::atomic<bool>> admitted,
		Fn<void(SyncConfigPublishDiskReply)> reply)
: _input(std::move(input))
, _admitted(std::move(admitted))
, _reply(std::move(reply))
, _routing(_input.routing) {
}

bool SyncConfigPublishFlow::current() const {
	return Worker() && !_done && _admitted && _admitted->load();
}

bool SyncConfigPublishFlow::setupAllowed() {
	const auto &setup = _input.setup;
	if (setup && (setup->scope.origin.account != _input.account
		|| setup->scope.root != _input.routing.root
		|| setup->scope.owner != _input.routing.owner
		|| setup->scope.mode != _routing.workMode(_input.account)
		|| setup->scope.binding != _input.binding)) { return false; }
	const auto path = QDir(_target->files.syncDirectory).filePath(u"sync-chat-setup.json"_q);
	const auto identity = SyncPathIdentity(path);
	if (identity.isEmpty()) { return false; }
	auto bytes = QByteArray();
	if (identity != "missing") {
		if (!setup || setup->scope.origin.account != _input.account
			|| setup->scope.root != _input.routing.root
			|| ReadSyncPrivateFile(path, 512 * 1024, bytes) != SyncStoreStatus::Ready) {
			return false;
		}
		const auto journal = ParseDesktopSyncChatCreateJournal(bytes,
			DesktopSyncChatSetupAuthority(setup->scope), _input.account);
		if (!journal || journal->step != DesktopSyncChatCreateStep::Done
			|| journal->pending || journal->cancelled || !journal->chat
			|| *journal->chat != _input.chat || !setup->cache
			|| journal->markId != setup->cache->mark.markId
			|| journal->markDate != setup->cache->mark.markDate
			|| setup->cache->mark.chat != _input.chat) {
			return false;
		}
	}
	if (_input.chat != PeerToken{}) {
		if (!setup || !setup->cache || setup->scope.carry || setup->cache->carry
			|| !setup->cache->readerConsent
			|| setup->scope.origin.account != _input.account
			|| setup->cache->account != _input.account
			|| setup->cache->mark.chat != _input.chat
			|| setup->cache->loginBinding != setup->scope.loginBinding) {
			return false;
		}
		const auto disk = OpenDesktopSyncChatDisk(_input.routing.root,
			_target->files.syncDirectory, _target->files.syncStore,
			_input.account, _input.binding, setup->scope.loginBinding, _admitted, true);
		if (!disk->error.isEmpty() || !disk->cache
			|| disk->cache->mark != setup->cache->mark
			|| !disk->cache->readerConsent) { return false; }
	}
	_setupPath = path;
	_setupIdentity = identity;
	_setupBytes = bytes;
	return current() && identity == SyncPathIdentity(path);
}

bool SyncConfigPublishFlow::recoveryAllowed() {
	const auto discovered = DiscoverDesktopSyncStores(_input.routing.root);
	if (!discovered) { return false; }
	auto input = SyncAccountRecoveryInput();
	input.account = _input.account;
	input.startupOwner = _input.routing.owner;
	input.signedIn = true;
	input.preferenceToken = _input.binding;
	input.otherPreferences = _input.otherPreferences;
	input.stores = discovered->stores;
	input.local = ReadSyncSettingsFile(_target->files);
	input.discoveryComplete = true;
	input.cloud = SyncRecoveryCloud{ _input.account,
		std::all_of(input.stores.begin(), input.stores.end(), [](const auto &store) {
			return !store.statePresent || bool(ParseSyncLocalState(store.state));
		}), _input.inventory };
	const auto plan = PlanSyncAccountRecovery(input);
	const auto recovery = QDir(_target->files.syncDirectory).filePath(u"recovery.json"_q);
	if (SyncPathIdentity(recovery) != "missing") { return false; }
	for (const auto &[path, identity] : discovered->identities) {
		if (identity.isEmpty() || SyncPathIdentity(path) != identity) { return false; }
	}
	return current() && (plan.status == SyncAccountRecoveryStatus::Setup
		|| plan.status == SyncAccountRecoveryStatus::Bound);
}

void SyncConfigPublishFlow::start() {
	if (_store || !current() || !_input.routing.ready
		|| _input.inventory.accountUserId != uint64_t(_input.account.userId)
		|| !PeerTokenText(_input.chat)
		|| _input.queue != SyncConfigSendQueue::Empty) {
		finish({ SyncConfigPublishStatus::NeedsReview }); return;
	}
	_opening = _routing.openTarget(_input.account, SettingsOpenRole::Own);
	_target = _routing.operationTarget(_input.account, false, _input.request.pendingOnly);
	if (!_opening || !_target || !recoveryAllowed()) {
		finish({ SyncConfigPublishStatus::NeedsReview }); return;
	}
	_store = std::make_unique<SyncLocalStore>(_target->files.syncDirectory);
	const auto opened = _store->Open(true);
	if (opened.status != SyncStoreStatus::Ready && opened.status != SyncStoreStatus::Uninitialized) {
		finish({ SyncConfigPublishStatus::StoreError, opened.status }); return;
	}
	_target = _routing.operationTarget(_input.account, false, _input.request.pendingOnly);
	_opening = _routing.openTarget(_input.account, SettingsOpenRole::Own);
	if (!_target || !_opening || !setupAllowed()) {
		finish({ SyncConfigPublishStatus::NeedsReview }); return;
	}
	if (opened.status == SyncStoreStatus::Uninitialized) {
		const auto &inventory = _input.inventory;
		const auto complete = inventory.status == SyncAccountInventoryStatus::Complete
			&& inventory.scan.complete() && inventory.read && inventory.read->complete()
			&& inventory.directory.complete && !inventory.directory.unreadableCandidate
			&& !inventory.directory.messageIdCollision;
		const auto empty = complete && inventory.read->records.empty()
			&& inventory.scan.candidateIds.empty() && !inventory.directory.selectedSpace
			&& inventory.directory.canCreateSpace;
		const auto &directory = inventory.directory;
		if (_input.request.pendingOnly || !complete
			|| (directory.selectedSpace && (!directory.publishableSpace
				|| *directory.selectedSpace != *directory.publishableSpace
				|| !IsSyncSpaceId(*directory.selectedSpace)))
			|| (_input.chat != PeerToken{} && !empty)
			|| (!inventory.directory.selectedSpace && !inventory.directory.canCreateSpace)) {
			finish({ SyncConfigPublishStatus::NeedsReview }); return;
		}
		auto initial = SyncLocalState();
		const auto install = FormatSyncInstallId(RandomBytes(16));
		const auto binding = PlanSyncSetupBindingToken(_input.binding, RandomBytes(16));
		const auto space = inventory.directory.selectedSpace
			? inventory.directory.selectedSpace
			: FormatTimeOrderedSyncSpaceId(uint64_t(QDateTime::currentMSecsSinceEpoch()), RandomBytes(10));
		if (!install || !space || (binding.status != SyncSetupBindingStatus::ReuseExisting
			&& binding.status != SyncSetupBindingStatus::CreateNew)) {
			finish({ SyncConfigPublishStatus::NeedsReview }); return;
		}
		initial.version = _input.chat == PeerToken{} ? 1 : 2;
		initial.chat = _input.chat;
		initial.space = *space;
		initial.install = *install;
		initial.createdDevice = u"desktop:"_q + *install;
		initial.bindingToken = binding.token;
		if (!current() || !_routing.validateOpen(*_opening)) {
			finish({ SyncConfigPublishStatus::AccountUnavailable }); return;
		}
		const auto initialized = _store->Initialize(initial);
		if (!initialized) { finish({ SyncConfigPublishStatus::StoreError, initialized.status }); return; }
		_bindingPending = true;
		_reply({ .step = SyncConfigPublishDiskStep::Binding, .initial = initial });
		return;
	}
	prepare();
}

void SyncConfigPublishFlow::bindingCommitted(QByteArray token) {
	if (!current() || !_bindingPending || !_store || !_store->state()
		|| CheckSyncAccountBinding(*_store->state(), token) != SyncAccountBindingVerdict::Bound) {
		finish({ SyncConfigPublishStatus::AccountUnbound }); return;
	}
	if (_setupIdentity != "missing") {
		if (!_input.setup || !current() || !_routing.validateOpen(*_opening)
			|| SyncPathIdentity(_setupPath) != _setupIdentity) {
			finish({ SyncConfigPublishStatus::NeedsReview }); return;
		}
		auto bytes = QByteArray();
		if (ReadSyncPrivateFile(_setupPath, 512 * 1024, bytes) != SyncStoreStatus::Ready
			|| bytes != _setupBytes) { finish({ SyncConfigPublishStatus::NeedsReview }); return; }
		auto journal = ParseDesktopSyncChatCreateJournal(bytes,
			DesktopSyncChatSetupAuthority(_input.setup->scope), _input.account);
		if (!journal || journal->step != DesktopSyncChatCreateStep::Done
			|| journal->pending || journal->cancelled || journal->chat != _input.chat) {
			finish({ SyncConfigPublishStatus::NeedsReview }); return;
		}
		auto scope = _input.setup->scope;
		scope.binding = token;
		journal->authority = DesktopSyncChatSetupAuthority(scope);
		const auto rebound = SerializeDesktopSyncChatCreateJournal(*journal);
		if (!rebound || !current()
			|| WriteSyncPrivateFile(_setupPath, *rebound) != SyncStoreStatus::Ready) {
			finish({ SyncConfigPublishStatus::NeedsReview }); return;
		}
		_input.setup->scope = std::move(scope);
		_setupBytes = *rebound;
		_setupIdentity = SyncPathIdentity(_setupPath);
	}
	_bindingPending = false;
	_input.binding = std::move(token);
	prepare();
}

bool SyncConfigPublishFlow::authorize() {
	if (!current() || !_store || !_store->state() || !_opening || !_target
		|| !_routing.validateOpen(*_opening) || !_routing.validateOperation(*_target)
		|| _bindingPending || _storeInput.state != SettingsFileState::Readable
		|| ReadSettingsFileInput(_target->files.syncStore) != _storeInput
		|| CheckSyncAccountBinding(*_store->state(), _input.binding) != SyncAccountBindingVerdict::Bound
		|| !DesktopSyncChatLedgerMatches(*_store->state(), _input.chat)
		|| SyncPathIdentity(_setupPath) != _setupIdentity) { return false; }
	if (_setupIdentity != "missing") {
		auto bytes = QByteArray();
		if (ReadSyncPrivateFile(_setupPath, 512 * 1024, bytes) != SyncStoreStatus::Ready
			|| bytes != _setupBytes) { return false; }
	}
	if (_input.setup) {
		const auto &scope = _input.setup->scope;
		const auto disk = OpenDesktopSyncChatDisk(_input.routing.root,
			_target->files.syncDirectory, _target->files.syncStore,
			_input.account, _input.binding, scope.loginBinding, _admitted, true);
		if (!disk->error.isEmpty() || !_input.setup->cache || !disk->cache
			|| disk->cache->mark != _input.setup->cache->mark || disk->cache->carry
			|| (_input.chat != PeerToken{} && !disk->cache->readerConsent)) { return false; }
	}
	return current();
}

void SyncConfigPublishFlow::prepare() {
	if (!current() || !_store || !_store->state()) {
		finish({ SyncConfigPublishStatus::AccountUnavailable }); return;
	}
	auto state = _store->state();
	if (!DesktopSyncChatLedgerMatches(*state, _input.chat)) {
		finish({ SyncConfigPublishStatus::NeedsReview }); return;
	}
	if (CheckSyncAccountBinding(*state, _input.binding) != SyncAccountBindingVerdict::Bound) {
		finish({ SyncConfigPublishStatus::AccountUnbound }); return;
	}
	if (_input.inventory.read) {
		for (const auto &record : _input.inventory.read->records) {
			(void)_store->SupplyRetainedConfigBase(record.bytes);
		}
	}
	if (!state->config.pendingSeq) {
		const auto retained = _store->CheckpointRetainedConfigBases();
		if (retained != SyncStoreStatus::Ready) {
			finish({ SyncConfigPublishStatus::StoreError, retained }); return;
		}
		state = _store->state();
	}
	_storeInput = ReadSettingsFileInput(_target->files.syncStore);
	if (!authorize()) { finish({ SyncConfigPublishStatus::AccountUnavailable }); return; }
	if (_input.initializeOnly) { finish({ SyncConfigPublishStatus::AlreadySynced }); return; }
	auto staged = QByteArray();
	if (state->config.pendingSeq) {
		const auto pending = _store->ReadPendingConfig();
		if (!pending) { finish({ SyncConfigPublishStatus::StoreError, pending.status }); return; }
		staged = pending.staged;
		if (!AccountSyncRecordSupported(staged)) { finish({ SyncConfigPublishStatus::NeedsReview }); return; }
	}
	const auto entry = PlanSyncConfigPublishEntry(_input.request, state->config.pendingSeq != 0);
	const auto local = entry == SyncConfigPublishEntry::NewContent
		? ReadSyncSettingsFile(_target->files) : SyncSettingsFile();
	const auto plan = PlanSyncConfigPost(*state, _input.binding, staged, _input.inventory,
		local, _input.request, _input.now, _input.writer, _input.queue);
	if (!authorize()) { finish({ SyncConfigPublishStatus::AccountUnavailable }); return; }
	switch (plan.step) {
	case SyncConfigPostStep::Finish: finish({ plan.status }); return;
	case SyncConfigPostStep::ConfirmFound: confirm(plan.record, plan.messageId); return;
	case SyncConfigPostStep::Stage: {
		const auto result = _store->StageConfig(plan.record, plan.nextConfigData, _input.binding);
		if (!result) { finish({ SyncConfigPublishStatus::StoreError, result.status }); return; }
		_storeInput = ReadSettingsFileInput(_target->files.syncStore);
		const auto next = PlanSyncConfigStagedPost(*_store->state(), _input.binding, plan.own, result.staged);
		if (next.step != SyncConfigPostStep::Post) { finish({ next.status }); return; }
		post(next.record);
	} return;
	case SyncConfigPostStep::Post: post(plan.record); return;
	}
}

void SyncConfigPublishFlow::post(QByteArray staged) {
	if (!authorize()) { finish({ SyncConfigPublishStatus::AccountUnavailable }); return; }
	_reply({ .step = SyncConfigPublishDiskStep::Post,
		.directory = _store->directory(), .staged = std::move(staged) });
}

void SyncConfigPublishFlow::confirm(QByteArray readback, int32_t messageId) {
	if (!authorize() || messageId <= 0) {
		finish({ SyncConfigPublishStatus::OutcomeUnknown }); return;
	}
	const auto result = _store->ConfirmConfigReadBack(readback,
		_store->state()->createdDevice, messageId, _input.chat);
	finish(result ? SyncConfigPublishResult{ SyncConfigPublishStatus::Confirmed, {}, messageId }
		: SyncConfigPublishResult{ SyncConfigPublishStatus::StoreError, result.status });
}

void SyncConfigPublishFlow::cancel() {
	if (_admitted) { _admitted->store(false); }
	_store.reset();
	_done = true;
}

void SyncConfigPublishFlow::finish(SyncConfigPublishResult result) {
	if (_done) { return; }
	_done = true;
	_store.reset();
	if (_reply) { _reply({ .result = result }); }
}

}

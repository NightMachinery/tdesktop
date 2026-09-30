/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_config_publish.h"

#include "base/unixtime.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "purple/purple_config_payload.h"
#include "purple/purple_sync_account_binding.h"
#include "purple/purple_sync_config_review.h"

#include <QtCore/QCryptographicHash>

#include <algorithm>
#include <limits>
#include <utility>

namespace Purple {
namespace {

constexpr auto kMaximumRecordBytes = 256 * 1024;

[[nodiscard]] QString RecordHash(const QByteArray &bytes) {
	return QString::fromLatin1(QCryptographicHash::hash(
		bytes, QCryptographicHash::Sha256).toHex());
}

[[nodiscard]] SyncConfigPublishStatus OwnFailure(
		SyncOwnInventoryStatus status) {
	switch (status) {
	case SyncOwnInventoryStatus::Incomplete:
		return SyncConfigPublishStatus::Incomplete;
	case SyncOwnInventoryStatus::CloneDetected:
		return SyncConfigPublishStatus::CloneDetected;
	default:
		return SyncConfigPublishStatus::NeedsReview;
	}
}

} // namespace

SyncConfigPublish::SyncConfigPublish(
		Main::Account &account,
		Main::Session &session,
		SyncAccountInventoryResult inventory,
		Fn<void(SyncConfigPublishResult)> finished,
		SyncConfigPublishRequest request)
: _account(base::make_weak(&account))
, _session(base::make_weak(&session))
, _inventory(std::move(inventory))
, _finished(std::move(finished))
, _request(std::move(request)) {
}

SyncConfigPublish::~SyncConfigPublish() {
	_done = true;
	if (_post) {
		_post->Cancel();
	}
}

bool SyncConfigPublish::AccountAvailable() const {
	const auto account = _account.get();
	const auto session = _session.get();
	return account
		&& session
		&& SyncAccountAvailable(*account, *session, _inventory.accountUserId);
}

void SyncConfigPublish::Start() {
	if (_started || _done) {
		return;
	}
	_started = true;
	if (!AccountAvailable()) {
		Finish({ SyncConfigPublishStatus::AccountUnavailable });
		return;
	}
	_store = std::make_unique<SyncLocalStore>(
		ConfigDirectory() + u"/sync"_q);
	const auto opened = _store->Open(true);
	if (opened.status != SyncStoreStatus::Ready || !_store->state()) {
		Finish({ SyncConfigPublishStatus::StoreError,
			opened.status == SyncStoreStatus::Ready
				? SyncStoreStatus::InvalidState : opened.status });
		return;
	}
	const auto state = _store->state();
	const auto account = _account.get();
	if (CheckAccountSyncBinding(*state, *account)
			!= SyncAccountBindingVerdict::Bound) {
		Finish({ SyncConfigPublishStatus::AccountUnbound });
		return;
	}
	const auto token = AccountSyncBindingToken(*account);
	auto staged = QByteArray();
	if (state->config.pendingSeq) {
		const auto pending = _store->ReadPendingConfig();
		if (!pending) {
			Finish({ SyncConfigPublishStatus::StoreError, pending.status });
			return;
		}
		staged = pending.staged;
	}
	auto inventory = _inventory;
	SelectSyncSpaceIfEmpty(inventory, state->space);
	const auto own = ReconcileOwnConfigInventory(
		*state, inventory, inventory.accountUserId, staged);
	if (own.status == SyncOwnInventoryStatus::PendingFound) {
		if (!own.pendingMessageId) {
			Finish({ SyncConfigPublishStatus::NeedsReview });
			return;
		}
		const auto found = std::find_if(
			_inventory.read->records.begin(),
			_inventory.read->records.end(),
			[&](const SyncCandidateRecord &record) {
				return record.id == *own.pendingMessageId
					&& record.status == SyncCandidateStatus::Valid
					&& record.bytes == staged;
			});
		if (found == _inventory.read->records.end()) {
			Finish({ SyncConfigPublishStatus::NeedsReview });
			return;
		}
		const auto confirmed = _store->ConfirmConfigReadBack(
			found->bytes, state->createdDevice, found->id);
		Finish(confirmed
			? SyncConfigPublishResult{
				SyncConfigPublishStatus::Confirmed,
				std::nullopt,
				found->id }
			: SyncConfigPublishResult{
				SyncConfigPublishStatus::StoreError,
				confirmed.status });
		return;
	}
	if (own.status != SyncOwnInventoryStatus::Absent
		&& own.status != SyncOwnInventoryStatus::Present) {
		Finish({ OwnFailure(own.status) });
		return;
	}
	if (!staged.isEmpty()) {
		PlanPost(*state, own, staged, token);
		return;
	}
	const auto local = ReadSyncSettingsFile(SettingsFilePath());
	if (local.status != SyncSettingsFileStatus::Present
		|| local.text.isEmpty()) {
		Finish({ SyncConfigPublishStatus::InvalidSettings });
		return;
	}
	if (_request.expectedFingerprint
		&& *_request.expectedFingerprint != local.fingerprint) {
		Finish({ SyncConfigPublishStatus::NeedsReview });
		return;
	}
	const auto gate = PlanSyncConfigPublishGate(
		*state,
		inventory,
		own,
		local.fingerprint,
		_request.expectedParents);
	if (gate.status == SyncConfigPublishGateStatus::AlreadySynced) {
		Finish({ SyncConfigPublishStatus::AlreadySynced });
		return;
	} else if (gate.status != SyncConfigPublishGateStatus::Proceed) {
		Finish({ SyncConfigPublishStatus::NeedsReview });
		return;
	}
	if (state->config.seq == std::numeric_limits<uint64_t>::max()) {
		Finish({ SyncConfigPublishStatus::NeedsReview });
		return;
	}
	const auto now = base::unixtime::now();
	if (now <= 0) {
		Finish({ SyncConfigPublishStatus::NeedsReview });
		return;
	}
	const auto built = BuildConfigRecord({
		.text = local.text,
		.parents = gate.parents,
		.space = state->space,
		.install = state->install,
		.device = state->createdDevice,
		.platform = SyncWriterPlatform(),
		.app = SyncWriterApp(),
		.seq = state->config.seq + 1,
		.at = uint64_t(now),
	});
	if (!built || built.canonical.size() > kMaximumRecordBytes) {
		Finish({ SyncConfigPublishStatus::InvalidSettings });
		return;
	}
	auto policy = SyncPublishPolicy();
	policy.enabled = true;
	auto observations = SyncPublishObservations();
	observations.ready = true;
	observations.discoveryComplete = true;
	observations.ownRecord = own.observation;
	observations.ownHead = own.head;
	const auto plan = PlanSyncPublish(
		*state, token, state->createdDevice, SyncLocalStream::Config,
		built.payloadHash, policy, observations);
	if (plan.action != SyncPublishAction::ReserveAndStage) {
		Finish({ SyncConfigPublishStatus::NeedsReview });
		return;
	}
	auto nextConfig = state->configData;
	nextConfig.pending = built.version.key;
	const auto stagedResult = _store->StageConfig(
		built.canonical, nextConfig, token);
	if (!stagedResult) {
		Finish({ SyncConfigPublishStatus::StoreError, stagedResult.status });
		return;
	}
	PlanPost(*_store->state(), own, stagedResult.staged, token);
}

void SyncConfigPublish::PlanPost(
		const SyncLocalState &state,
		const SyncOwnInventoryResult &own,
		const QByteArray &staged,
		const QByteArray &token) {
	if (!AccountAvailable()) {
		Finish({ SyncConfigPublishStatus::AccountUnavailable });
		return;
	}
	auto policy = SyncPublishPolicy();
	policy.enabled = true;
	policy.editEnabled = false;
	auto observations = SyncPublishObservations();
	observations.discoveryComplete = true;
	observations.ready = true;
	observations.attempt = SyncPublishAttempt::ReconciledAbsent;
	observations.stagedRecordMatches = true;
	observations.stagedRecordHash = RecordHash(staged);
	observations.ownRecord = own.observation;
	observations.ownHead = own.head;
	const auto plan = PlanSyncPublish(
		state, token, state.createdDevice, SyncLocalStream::Config,
		state.config.ownHash, policy, observations);
	if (plan.action != SyncPublishAction::Post) {
		Finish({ SyncConfigPublishStatus::NeedsReview });
		return;
	}
	const auto weak = base::make_weak(this);
	_post = std::make_unique<SyncConfigPost>(
		not_null<Main::Session*>{ _session.get() },
		staged,
		[weak](SyncConfigPostResult result) {
			if (const auto self = weak.get(); self && !self->_done) {
				self->OnPost(std::move(result));
			}
		});
	_posted = true;
	_post->Start();
}

void SyncConfigPublish::OnPost(SyncConfigPostResult result) {
	if (result.status == SyncConfigPostStatus::Confirmed) {
		if (!AccountAvailable()) {
			Finish({ SyncConfigPublishStatus::OutcomeUnknown });
			return;
		}
		const auto confirmed = _store->ConfirmConfigReadBack(
			result.readback,
			_store->state()->createdDevice,
			int32_t(result.messageId.bare));
		Finish(confirmed
			? SyncConfigPublishResult{
				SyncConfigPublishStatus::Confirmed,
				std::nullopt,
				int32_t(result.messageId.bare) }
			: SyncConfigPublishResult{
				SyncConfigPublishStatus::StoreError,
				confirmed.status });
	} else {
		Finish({
			result.status == SyncConfigPostStatus::NeedsReview
				|| result.status == SyncConfigPostStatus::InvalidRecord
				? SyncConfigPublishStatus::NeedsReview
				: SyncConfigPublishStatus::OutcomeUnknown,
		});
	}
}

void SyncConfigPublish::Cancel() {
	if (_done) {
		return;
	}
	if (_post) {
		_post->Cancel();
	} else {
		Finish({ _posted
			? SyncConfigPublishStatus::OutcomeUnknown
			: SyncConfigPublishStatus::Cancelled });
	}
}

void SyncConfigPublish::Finish(SyncConfigPublishResult result) {
	if (_done) {
		return;
	}
	_done = true;
	_store.reset();
	if (auto finished = std::move(_finished)) {
		finished(std::move(result));
	}
}

} // namespace Purple

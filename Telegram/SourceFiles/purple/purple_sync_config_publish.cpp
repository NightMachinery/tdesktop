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
#include "purple/purple_sync_account_binding.h"

#include <utility>

namespace Purple {

SyncConfigPublish::SyncConfigPublish(
		Main::Account &account,
		Main::Session &session,
		SyncAccountInventoryResult inventory,
		SyncConfigSendQueue scanQueue,
		Fn<void(SyncConfigPublishResult)> finished,
		SyncConfigPublishRequest request)
: _account(base::make_weak(&account))
, _session(base::make_weak(&session))
, _inventory(std::move(inventory))
, _scanQueue(scanQueue)
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
	const auto entry = PlanSyncConfigPublishEntry(
		_request,
		state->config.pendingSeq != 0);
	if (entry == SyncConfigPublishEntry::Refuse) {
		Finish({ SyncConfigPublishStatus::NeedsReview });
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
	const auto local = (entry == SyncConfigPublishEntry::NewContent)
		? ReadSyncSettingsFile(SettingsFilePath())
		: SyncSettingsFile();
	const auto queue = (_scanQueue == SyncConfigSendQueue::HoldsSyncRecord)
		? SyncConfigSendQueue::HoldsSyncRecord
		: SyncConfigSendQueueOf(not_null<Main::Session*>{ _session.get() });
	const auto plan = PlanSyncConfigPost(
		*state,
		token,
		staged,
		_inventory,
		local,
		_request,
		int64_t(base::unixtime::now()),
		SyncWriter(),
		queue);
	switch (plan.step) {
	case SyncConfigPostStep::Finish:
		Finish({ plan.status });
		return;
	case SyncConfigPostStep::ConfirmFound: {
		const auto confirmed = _store->ConfirmConfigReadBack(
			plan.record,
			state->createdDevice,
			plan.messageId);
		Finish(confirmed
			? SyncConfigPublishResult{
				SyncConfigPublishStatus::Confirmed,
				std::nullopt,
				plan.messageId }
			: SyncConfigPublishResult{
				SyncConfigPublishStatus::StoreError,
				confirmed.status });
	} return;
	case SyncConfigPostStep::Stage: {
		const auto stagedResult = _store->StageConfig(
			plan.record,
			plan.nextConfigData,
			token);
		if (!stagedResult) {
			Finish({
				SyncConfigPublishStatus::StoreError,
				stagedResult.status,
			});
			return;
		}
		PlanPost(*_store->state(), plan.own, stagedResult.staged, token);
	} return;
	case SyncConfigPostStep::Post:
		StartPost(plan.record);
		return;
	}
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
	const auto plan = PlanSyncConfigStagedPost(state, token, own, staged);
	if (plan.step != SyncConfigPostStep::Post) {
		Finish({ plan.status });
		return;
	}
	StartPost(plan.record);
}

void SyncConfigPublish::StartPost(const QByteArray &staged) {
	if (!AccountAvailable()) {
		Finish({ SyncConfigPublishStatus::AccountUnavailable });
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

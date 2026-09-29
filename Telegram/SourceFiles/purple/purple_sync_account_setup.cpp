/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_account_setup.h"

#include "base/random.h"
#include "base/unixtime.h"
#include "data/data_user.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "purple/purple_sync_account_binding.h"

#include <QtCore/QDateTime>

namespace Purple {
namespace {

[[nodiscard]] QByteArray RandomBytes(int size) {
	auto bytes = QByteArray(size, '\0');
	base::RandomFill(bytes.data(), bytes.size());
	return bytes;
}

} // namespace

SyncAccountSetupResult InitializeSyncAccountLocally(
		Main::Account &account,
		Main::Session &session,
		const SyncAccountInventoryResult &inventory) {
	if (account.loggingOut() || account.maybeSession() != &session) {
		return { SyncAccountSetupStatus::AccountUnavailable };
	}
	if (!inventory.accountUserId
		|| inventory.accountUserId != peerToUser(session.user()->id).bare) {
		return { SyncAccountSetupStatus::AccountUnavailable };
	}
	if (!inventory.scan.complete()
		|| !inventory.read
		|| inventory.read->status == SyncCandidateReadStatus::Incomplete
		|| !inventory.directory.complete) {
		return { SyncAccountSetupStatus::IncompleteInventory };
	}
	if (inventory.status == SyncAccountInventoryStatus::NeedsReview
		|| inventory.read->status == SyncCandidateReadStatus::NeedsReview) {
		return { SyncAccountSetupStatus::NeedsReview };
	}
	if (inventory.status != SyncAccountInventoryStatus::Complete
		|| !inventory.read->complete()) {
		return { SyncAccountSetupStatus::IncompleteInventory };
	}
	const auto &directory = inventory.directory;
	if (directory.unreadableCandidate
		|| directory.messageIdCollision
		|| (directory.selectedSpace
			&& (!directory.publishableSpace
				|| *directory.selectedSpace != *directory.publishableSpace
				|| !IsSyncSpaceId(*directory.selectedSpace)))
		|| (!directory.selectedSpace && !directory.canCreateSpace)) {
		return { SyncAccountSetupStatus::NeedsReview };
	}

	auto store = SyncLocalStore(ConfigDirectory() + u"/sync"_q);
	const auto opened = store.Open(true);
	if (opened.status != SyncStoreStatus::Ready
		&& opened.status != SyncStoreStatus::Uninitialized) {
		return { SyncAccountSetupStatus::StoreError, opened.status };
	}
	if (opened.status == SyncStoreStatus::Ready) {
		const auto state = store.state();
		if (!state) {
			return { SyncAccountSetupStatus::StoreError,
				SyncStoreStatus::InvalidState };
		}
		if (CheckAccountSyncBinding(*state, account)
			!= SyncAccountBindingVerdict::Bound) {
			return { SyncAccountSetupStatus::AccountUnbound };
		}
		if (!directory.selectedSpace || state->space != *directory.selectedSpace) {
			return { SyncAccountSetupStatus::NeedsReview };
		}
		return { SyncAccountSetupStatus::AlreadyBound,
			std::nullopt, state->space, false };
	}

	auto initial = SyncLocalState();
	const auto install = FormatSyncInstallId(RandomBytes(16));
	const auto bindingToken = FormatSyncBindingToken(RandomBytes(16));
	const auto seconds = base::unixtime::now();
	const auto millis = QDateTime::currentMSecsSinceEpoch();
	const auto newSpace = directory.selectedSpace
		? directory.selectedSpace
		: (seconds > 0 && millis > 0)
		? FormatTimeOrderedSyncSpaceId(
			uint64_t(seconds) * 1000 + uint64_t(millis % 1000),
			RandomBytes(10))
		: std::nullopt;
	if (!install || !bindingToken || !newSpace) {
		return { SyncAccountSetupStatus::InvalidGeneratedState };
	}
	initial.install = *install;
	initial.createdDevice = u"desktop:"_q + *install;
	initial.space = *newSpace;
	initial.bindingToken = *bindingToken;
	if (initial.createdDevice.toUtf8().size() > 256) {
		return { SyncAccountSetupStatus::InvalidGeneratedState };
	}
	if (CheckAccountSyncBinding(initial, account)
		!= SyncAccountBindingVerdict::MissingAccountToken) {
		return { SyncAccountSetupStatus::AccountUnbound };
	}
	const auto initialized = store.Initialize(initial);
	if (!initialized) {
		return { SyncAccountSetupStatus::StoreError, initialized.status };
	}
	if (!StoreAccountSyncBinding(initial, account)
		|| CheckAccountSyncBinding(initial, account)
			!= SyncAccountBindingVerdict::Bound) {
		return { SyncAccountSetupStatus::AccountUnbound };
	}
	return { SyncAccountSetupStatus::Ready,
		std::nullopt, initial.space, !directory.selectedSpace };
}

} // namespace Purple

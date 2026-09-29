/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_sync_account_inventory.h"
#include "purple/purple_sync_local_state.h"

#include <QtCore/QByteArray>

#include <cstdint>
#include <optional>
#include <vector>

namespace Purple {

enum class SyncOwnInventoryStatus {
	Incomplete,
	NeedsReview,
	Absent,
	Present,
	PendingFound,
	CloneDetected,
};

struct SyncOwnInventoryResult {
	SyncOwnInventoryStatus status = SyncOwnInventoryStatus::Incomplete;
	OwnRecordObservation observation;
	SyncPublishReadBack head;
	std::optional<int32_t> pendingMessageId;
	SyncCloneVerdict cloneVerdict = SyncCloneVerdict::PendingReconcile;
	std::vector<int32_t> duplicateHeadMessageIds;
};

[[nodiscard]] SyncOwnInventoryResult ReconcileOwnConfigInventory(
	const SyncLocalState &state,
	const SyncAccountInventoryResult &inventory,
	uint64_t expectedAccountUserId,
	const QByteArray &stagedCanonicalRecord = {});

} // namespace Purple

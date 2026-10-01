/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_config_diff.h"
#include "purple/purple_sync_config_apply.h"
#include "purple/purple_sync_config_history.h"
#include "purple/purple_sync_config_review.h"

#include <QtCore/QDateTime>
#include <QtCore/QString>

#include <optional>
#include <vector>

namespace Purple {

using SyncConfigBoxAction = SyncConfigAction;

struct SyncConfigBoxStatus {
	QString text;
	SyncConfigBoxAction action = SyncConfigBoxAction::None;
};

inline constexpr auto kSyncConfigDiffLineLimit = 400;
inline constexpr auto kSyncConfigSummaryLimit = 6;

[[nodiscard]] QString SyncCloudDisclosureText();
[[nodiscard]] QString SyncDeviceList(std::vector<QString> names);
[[nodiscard]] QString SyncRecordTimeText(uint64_t seconds);

[[nodiscard]] std::vector<QString> SyncOfferedDeviceNames(
	const SyncConfigReview &review);
[[nodiscard]] bool SyncSettingsPublishable(const SyncConfigReview &review);
[[nodiscard]] bool SyncSettingsMatchBase(const SyncConfigReview &review);

[[nodiscard]] SyncConfigBoxStatus DescribeSyncConfigReview(
	const SyncConfigReview &review,
	const QDateTime &checkedAt);
[[nodiscard]] QString SyncConfigBoxActionText(SyncConfigBoxAction action);
[[nodiscard]] QString SyncConfigChoiceIntro(const SyncConfigReview &review);
[[nodiscard]] QString SyncConfigChoiceButtonText(
	const SyncConfigReview &review,
	const std::optional<QString> &chosenRemoteKey);

[[nodiscard]] std::vector<QString> SyncConfigChangeLines(
	const ConfigChangeSummary &summary,
	const ConfigTextDiff &diff);
[[nodiscard]] QString SyncConfigDiffText(
	const ConfigTextDiff &diff,
	int lineLimit = kSyncConfigDiffLineLimit);

[[nodiscard]] QString SyncConfigHistoryRowText(
	const SyncConfigHistoryEntry &entry);
[[nodiscard]] QString SyncConfigHistoryMomentText(
	const SyncConfigHistoryEntry &entry);
[[nodiscard]] QString SyncConfigApplyFailureText(
	const SyncConfigApplyResult &result);
[[nodiscard]] bool SyncConfigUndoFinished(
	const SyncConfigRestoreResult &result);
[[nodiscard]] QString SyncConfigRestoreText(
	const SyncConfigRestoreResult &result,
	const SyncConfigHistoryEntry &entry,
	bool staysLocal = true);
[[nodiscard]] QString SyncConfigUndoneText(bool staysLocal);

} // namespace Purple

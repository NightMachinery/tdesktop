/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_config_sync.h"
#include "purple/purple_sync_account_inventory.h"
#include "purple/purple_sync_inventory.h"
#include "purple/purple_sync_local_store.h"

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QString>

#include <cstdint>
#include <optional>
#include <vector>

namespace Main {
class Account;
class Session;
}

namespace Purple {

enum class SyncSettingsFileStatus {
	Present,
	Absent,
	Invalid,
};

struct SyncSettingsFile {
	SyncSettingsFileStatus status = SyncSettingsFileStatus::Invalid;
	QByteArray text;
	QString fingerprint;
};

struct SyncConfigHeadRecord {
	ConfigHead head;
	int32_t messageId = 0;
	QByteArray text;
	QString device;
	QString platform;
	QString app;
	uint64_t at = 0;
	bool newerSchema = false;
};

enum class SyncConfigHeadsStatus {
	Complete,
	NeedsReview,
};

struct SyncConfigHeads {
	SyncConfigHeadsStatus status = SyncConfigHeadsStatus::NeedsReview;
	std::vector<SyncConfigHeadRecord> heads;
};

struct SyncConfigOwnHead {
	bool valid = false;
	std::optional<SyncConfigHeadRecord> head;
};

enum class SyncConfigReviewStatus {
	Ready,
	NeedsReview,
	Incomplete,
	CloneDetected,
	AccountUnavailable,
	AccountUnbound,
	StoreError,
	InvalidSettings,
};

struct SyncConfigReview {
	SyncConfigReviewStatus status = SyncConfigReviewStatus::Incomplete;
	std::optional<SyncStoreStatus> storeStatus;
	uint64_t accountUserId = 0;
	bool bound = false;
	QString space;
	ConfigSyncState state;
	SyncSettingsFile local;
	std::vector<SyncConfigHeadRecord> heads;
	std::optional<SyncConfigHeadRecord> ownHead;
	ConfigSyncPlan plan;
};

struct SyncConfigPublishRequest {
	bool pendingOnly = false;
	std::optional<QString> expectedFingerprint;
	std::optional<std::vector<QString>> expectedParents;
};

enum class SyncConfigPublishEntry {
	Refuse,
	FinishStaged,
	NewContent,
};

enum class SyncConfigPublishGateStatus {
	Proceed,
	AlreadySynced,
	NeedsReview,
};

struct SyncConfigPublishGate {
	SyncConfigPublishGateStatus status
		= SyncConfigPublishGateStatus::NeedsReview;
	ConfigSyncPlan plan;
	std::vector<ConfigVersion> parents;
};

[[nodiscard]] QString SyncDeviceName(
	const QString &platform,
	const QString &install);
[[nodiscard]] QString SyncDeviceName(const SyncConfigHeadRecord &record);
[[nodiscard]] QString SyncMomentText(const QDateTime &when);
[[nodiscard]] QString SyncWriterPlatform();
[[nodiscard]] QString SyncWriterApp();

[[nodiscard]] SyncSettingsFile ReadSyncSettingsFile(const QString &path);
[[nodiscard]] bool SameSyncSettingsFile(
	const SyncSettingsFile &a,
	const SyncSettingsFile &b);
[[nodiscard]] ConfigSyncState SyncConfigStateOf(const SyncLocalState &state);
[[nodiscard]] SyncLocalConfigState SyncLocalConfigOf(
	const ConfigSyncState &state);
[[nodiscard]] std::vector<ConfigHead> SyncConfigHeadsOf(
	const std::vector<SyncConfigHeadRecord> &records);
[[nodiscard]] std::vector<QString> SyncConfigVersionKeys(
	const std::vector<ConfigVersion> &versions);
[[nodiscard]] bool SameSyncConfigKeySet(
	std::vector<QString> a,
	std::vector<QString> b);
void SelectSyncSpaceIfEmpty(
	SyncAccountInventoryResult &inventory,
	const QString &space);

[[nodiscard]] std::optional<SyncConfigHeadRecord> ParseSyncConfigHead(
	const SyncCandidateRecord &record);
[[nodiscard]] SyncConfigHeads ExtractSyncConfigHeads(
	const SyncAccountInventoryResult &inventory,
	const QString &space,
	const QString &ownInstall);
[[nodiscard]] SyncConfigOwnHead FindSyncConfigOwnHead(
	const SyncAccountInventoryResult &inventory,
	const SyncOwnInventoryResult &own);

[[nodiscard]] SyncConfigReview ReviewSyncConfigInventory(
	const SyncAccountInventoryResult &inventory,
	const SyncLocalState *state,
	const QByteArray &stagedRecord,
	const SyncSettingsFile &local);
[[nodiscard]] SyncConfigReview ReviewSyncConfig(
	Main::Account &account,
	Main::Session &session,
	const SyncAccountInventoryResult &inventory);

[[nodiscard]] SyncConfigPublishEntry PlanSyncConfigPublishEntry(
	const SyncConfigPublishRequest &request,
	bool staged);
[[nodiscard]] SyncConfigPublishGate PlanSyncConfigPublishGate(
	const SyncLocalState &state,
	const SyncAccountInventoryResult &inventory,
	const SyncOwnInventoryResult &own,
	const QString &localFingerprint,
	const SyncConfigPublishRequest &request);

} // namespace Purple

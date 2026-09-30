/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_config_review.h"

#include "base/platform/base_platform_info.h"
#include "purple/purple_config.h"
#include "purple/purple_config_payload.h"
#include "purple/purple_state.h"
#include "purple/purple_sync_account_binding.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

#include <algorithm>

namespace Purple {
namespace {

constexpr auto kMaximumSettingsBytes = 256 * 1024;
constexpr auto kMaximumPlatformLength = 32;
constexpr auto kShortInstallLength = 4;

[[nodiscard]] SyncConfigReviewStatus InventoryStatus(
		const SyncAccountInventoryResult &inventory) {
	const auto &directory = inventory.directory;
	if (!inventory.scan.complete()
		|| !inventory.read
		|| inventory.read->status == SyncCandidateReadStatus::Incomplete
		|| !directory.complete) {
		return SyncConfigReviewStatus::Incomplete;
	} else if (inventory.status == SyncAccountInventoryStatus::NeedsReview
		|| inventory.read->status == SyncCandidateReadStatus::NeedsReview) {
		return SyncConfigReviewStatus::NeedsReview;
	} else if (inventory.status != SyncAccountInventoryStatus::Complete
		|| !inventory.read->complete()) {
		return SyncConfigReviewStatus::Incomplete;
	} else if (directory.unreadableCandidate
		|| directory.messageIdCollision
		|| (directory.selectedSpace
			&& (!directory.publishableSpace
				|| *directory.selectedSpace != *directory.publishableSpace
				|| !IsSyncSpaceId(*directory.selectedSpace)))
		|| (!directory.selectedSpace && !directory.canCreateSpace)) {
		return SyncConfigReviewStatus::NeedsReview;
	}
	return SyncConfigReviewStatus::Ready;
}

[[nodiscard]] QString RecordHash(const QByteArray &bytes) {
	return QString::fromLatin1(QCryptographicHash::hash(
		bytes,
		QCryptographicHash::Sha256).toHex());
}

[[nodiscard]] bool AnyNewerSchema(
		const std::vector<SyncConfigHeadRecord> &heads) {
	return std::any_of(heads.begin(), heads.end(), [](const auto &head) {
		return head.newerSchema;
	});
}

[[nodiscard]] SyncConfigReview Refuse(
		SyncConfigReview review,
		SyncConfigReviewStatus status) {
	review.status = status;
	return review;
}

} // namespace

QString SyncDeviceName(const QString &platform, const QString &install) {
	const auto dash = install.indexOf(u'-');
	const auto shortId = install.mid(dash + 1).left(kShortInstallLength);
	const auto trimmed = platform.simplified().left(kMaximumPlatformLength);
	const auto name = trimmed.isEmpty() ? u"Device"_q : trimmed;
	return shortId.isEmpty() ? name : (name + u' ' + shortId);
}

QString SyncDeviceName(const SyncConfigHeadRecord &record) {
	return SyncDeviceName(record.platform, record.head.install);
}

QString SyncMomentText(const QDateTime &when) {
	return when.toLocalTime().toString(u"yyyy-MM-dd HH:mm"_q);
}

QString SyncWriterPlatform() {
	return Platform::IsWindows()
		? u"Windows"_q
		: Platform::IsMac()
		? u"macOS"_q
		: u"Linux"_q;
}

QString SyncWriterApp() {
	return u"Purple Telegram Desktop"_q;
}

SyncSettingsFile ReadSyncSettingsFile(const QString &path) {
	auto result = SyncSettingsFile();
	const auto info = QFileInfo(path);
	if (!info.exists() && !info.isSymLink()) {
		result.status = SyncSettingsFileStatus::Absent;
		result.fingerprint = SettingsFingerprint(QByteArray());
		return result;
	} else if (info.isSymLink()
		|| !info.isFile()
		|| info.size() > kMaximumSettingsBytes) {
		return result;
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	const auto text = file.read(kMaximumSettingsBytes + 1);
	if (file.error() != QFileDevice::NoError
		|| text.size() > kMaximumSettingsBytes
		|| !file.atEnd()) {
		return result;
	}
	result.status = SyncSettingsFileStatus::Present;
	result.text = text;
	result.fingerprint = SettingsFingerprint(text);
	return result;
}

bool SameSyncSettingsFile(
		const SyncSettingsFile &a,
		const SyncSettingsFile &b) {
	return a.status == b.status
		&& a.status != SyncSettingsFileStatus::Invalid
		&& a.fingerprint == b.fingerprint
		&& a.text == b.text;
}

ConfigSyncState SyncConfigStateOf(const SyncLocalState &state) {
	const auto &data = state.configData;
	return {
		.space = state.space,
		.install = state.install,
		.base = data.base,
		.baseLineage = data.baseLineage,
		.equiv = data.equiv,
		.pending = data.pending,
		.seenSeq = data.seenSeq,
	};
}

SyncLocalConfigState SyncLocalConfigOf(const ConfigSyncState &state) {
	return {
		.base = state.base,
		.baseLineage = state.baseLineage,
		.equiv = state.equiv,
		.pending = state.pending,
		.seenSeq = state.seenSeq,
	};
}

std::vector<ConfigHead> SyncConfigHeadsOf(
		const std::vector<SyncConfigHeadRecord> &records) {
	auto result = std::vector<ConfigHead>();
	result.reserve(records.size());
	for (const auto &record : records) {
		result.push_back(record.head);
	}
	return result;
}

std::vector<QString> SyncConfigVersionKeys(
		const std::vector<ConfigVersion> &versions) {
	auto result = std::vector<QString>();
	result.reserve(versions.size());
	for (const auto &version : versions) {
		result.push_back(version.key);
	}
	return result;
}

bool SameSyncConfigKeySet(std::vector<QString> a, std::vector<QString> b) {
	std::sort(a.begin(), a.end());
	std::sort(b.begin(), b.end());
	a.erase(std::unique(a.begin(), a.end()), a.end());
	b.erase(std::unique(b.begin(), b.end()), b.end());
	return a == b;
}

void SelectSyncSpaceIfEmpty(
		SyncAccountInventoryResult &inventory,
		const QString &space) {
	auto &directory = inventory.directory;
	if (!directory.selectedSpace
		&& directory.canCreateSpace
		&& directory.groups.empty()) {
		directory.selectedSpace = space;
		directory.publishableSpace = space;
	}
}

std::optional<SyncConfigHeadRecord> ParseSyncConfigHead(
		const SyncCandidateRecord &record) {
	const auto parsed = ParseSyncEnvelope(record.bytes);
	if (!parsed || !parsed.header || parsed.header->stream != u"config"_q) {
		return std::nullopt;
	}
	const auto inspected = InspectConfigPayload(parsed);
	if (inspected.status == ConfigPayloadStatus::Invalid
		|| !IsConfigVersionKey(inspected.version.key)) {
		return std::nullopt;
	}
	const auto &header = *parsed.header;
	return SyncConfigHeadRecord{
		.head = {
			.space = header.space,
			.install = header.writerInstall,
			.seq = header.seq,
			.key = inspected.version.key,
			.lineage = inspected.version.lineage,
		},
		.messageId = record.id,
		.text = inspected.text,
		.device = header.writerDevice,
		.platform = header.writerPlatform,
		.app = header.writerApp,
		.at = header.at,
		.newerSchema = (inspected.status == ConfigPayloadStatus::NewerSchema),
	};
}

SyncConfigHeads ExtractSyncConfigHeads(
		const SyncAccountInventoryResult &inventory,
		const QString &space,
		const QString &ownInstall) {
	auto result = SyncConfigHeads();
	if (!inventory.read || space.isEmpty()) {
		return result;
	}
	const auto &records = inventory.read->records;
	for (const auto &group : inventory.directory.groups) {
		if (group.space != space
			|| group.stream != u"config"_q
			|| (!ownInstall.isEmpty() && group.install == ownInstall)) {
			continue;
		}
		if (group.ambiguous
			|| !group.supportedHead
			|| group.headCandidates.empty()) {
			return result;
		}
		const auto candidate = std::min_element(
			group.headCandidates.begin(),
			group.headCandidates.end(),
			[](const auto &a, const auto &b) {
				return a.messageId < b.messageId;
			});
		const auto record = std::find_if(
			records.begin(),
			records.end(),
			[&](const SyncCandidateRecord &record) {
				return record.id == candidate->messageId;
			});
		if (record == records.end()
			|| record->status != SyncCandidateStatus::Valid
			|| !candidate->header
			|| RecordHash(record->bytes) != candidate->recordHash) {
			return result;
		}
		const auto parsed = ParseSyncConfigHead(*record);
		if (!parsed
			|| parsed->head.space != group.space
			|| parsed->head.install != group.install
			|| parsed->head.seq != candidate->header->seq) {
			return result;
		}
		result.heads.push_back(*parsed);
	}
	result.status = SyncConfigHeadsStatus::Complete;
	return result;
}

SyncConfigOwnHead FindSyncConfigOwnHead(
		const SyncAccountInventoryResult &inventory,
		const SyncOwnInventoryResult &own) {
	if (own.status != SyncOwnInventoryStatus::Absent
		&& own.status != SyncOwnInventoryStatus::Present
		&& own.status != SyncOwnInventoryStatus::PendingFound) {
		return {};
	} else if (own.observation.kind == OwnRecordObservationKind::Absent) {
		return { .valid = true };
	} else if (own.observation.kind != OwnRecordObservationKind::Present
		|| !inventory.read) {
		return {};
	}
	const auto &records = inventory.read->records;
	const auto record = std::find_if(
		records.begin(),
		records.end(),
		[&](const SyncCandidateRecord &record) {
			return record.id == own.head.messageId;
		});
	if (record == records.end()
		|| record->status != SyncCandidateStatus::Valid) {
		return {};
	}
	const auto parsed = ParseSyncConfigHead(*record);
	if (!parsed
		|| parsed->newerSchema
		|| parsed->head.seq != own.observation.seq) {
		return {};
	}
	return { .valid = true, .head = parsed };
}

SyncConfigReview ReviewSyncConfigInventory(
		const SyncAccountInventoryResult &inventory,
		const SyncLocalState *state,
		const QByteArray &stagedRecord,
		const SyncSettingsFile &local) {
	auto result = SyncConfigReview();
	result.accountUserId = inventory.accountUserId;
	result.bound = (state != nullptr);
	result.local = local;
	if (local.status == SyncSettingsFileStatus::Invalid) {
		return Refuse(
			std::move(result),
			SyncConfigReviewStatus::InvalidSettings);
	}
	const auto inventoryStatus = InventoryStatus(inventory);
	if (inventoryStatus != SyncConfigReviewStatus::Ready) {
		return Refuse(std::move(result), inventoryStatus);
	}
	if (!state) {
		const auto &selected = inventory.directory.selectedSpace;
		if (!selected) {
			result.plan.verdict = ConfigSyncVerdict::Empty;
			result.status = SyncConfigReviewStatus::Ready;
			return result;
		}
		const auto heads = ExtractSyncConfigHeads(
			inventory,
			*selected,
			QString());
		if (heads.status != SyncConfigHeadsStatus::Complete
			|| AnyNewerSchema(heads.heads)) {
			return Refuse(
				std::move(result),
				SyncConfigReviewStatus::NeedsReview);
		}
		result.space = *selected;
		result.state.space = *selected;
		result.heads = heads.heads;
		result.plan = PlanConfigSync(
			local.fingerprint,
			result.state,
			SyncConfigHeadsOf(result.heads));
		result.status = SyncConfigReviewStatus::Ready;
		return result;
	}
	auto scoped = inventory;
	SelectSyncSpaceIfEmpty(scoped, state->space);
	result.space = state->space;
	result.state = SyncConfigStateOf(*state);
	if (!scoped.directory.selectedSpace
		|| *scoped.directory.selectedSpace != state->space) {
		return Refuse(std::move(result), SyncConfigReviewStatus::NeedsReview);
	}
	const auto own = ReconcileOwnConfigInventory(
		*state,
		scoped,
		scoped.accountUserId,
		stagedRecord);
	switch (own.status) {
	case SyncOwnInventoryStatus::Incomplete:
		return Refuse(std::move(result), SyncConfigReviewStatus::Incomplete);
	case SyncOwnInventoryStatus::CloneDetected:
		return Refuse(
			std::move(result),
			SyncConfigReviewStatus::CloneDetected);
	case SyncOwnInventoryStatus::NeedsReview:
		return Refuse(std::move(result), SyncConfigReviewStatus::NeedsReview);
	case SyncOwnInventoryStatus::Absent:
	case SyncOwnInventoryStatus::Present:
	case SyncOwnInventoryStatus::PendingFound:
		break;
	}
	const auto ownHead = FindSyncConfigOwnHead(scoped, own);
	const auto heads = ExtractSyncConfigHeads(
		scoped,
		state->space,
		state->install);
	if (!ownHead.valid
		|| heads.status != SyncConfigHeadsStatus::Complete
		|| AnyNewerSchema(heads.heads)) {
		return Refuse(std::move(result), SyncConfigReviewStatus::NeedsReview);
	}
	result.heads = heads.heads;
	result.ownHead = ownHead.head;
	result.plan = PlanConfigSync(
		local.fingerprint,
		result.state,
		SyncConfigHeadsOf(result.heads),
		ownHead.head
			? std::make_optional(ownHead.head->head)
			: std::nullopt);
	result.status = SyncConfigReviewStatus::Ready;
	return result;
}

SyncConfigReview ReviewSyncConfig(
		Main::Account &account,
		Main::Session &session,
		const SyncAccountInventoryResult &inventory) {
	auto result = SyncConfigReview();
	result.accountUserId = inventory.accountUserId;
	if (!SyncAccountAvailable(account, session, inventory.accountUserId)) {
		result.status = SyncConfigReviewStatus::AccountUnavailable;
		return result;
	}
	const auto local = ReadSyncSettingsFile(SettingsFilePath());
	const auto root = ConfigDirectory() + u"/sync"_q;
	const auto stateInfo = QFileInfo(root + u"/state.json"_q);
	if (!stateInfo.exists() && !stateInfo.isSymLink()) {
		return ReviewSyncConfigInventory(inventory, nullptr, {}, local);
	}
	auto store = SyncLocalStore(root);
	const auto opened = store.Open(true);
	if (opened.status == SyncStoreStatus::Uninitialized) {
		return ReviewSyncConfigInventory(inventory, nullptr, {}, local);
	}
	const auto state = store.state();
	if (opened.status != SyncStoreStatus::Ready || !state) {
		result.status = SyncConfigReviewStatus::StoreError;
		result.storeStatus = (opened.status == SyncStoreStatus::Ready)
			? SyncStoreStatus::InvalidState
			: opened.status;
		return result;
	}
	if (CheckAccountSyncBinding(*state, account)
			!= SyncAccountBindingVerdict::Bound) {
		result.status = SyncConfigReviewStatus::AccountUnbound;
		return result;
	}
	auto staged = QByteArray();
	if (state->config.pendingSeq) {
		const auto pending = store.ReadPendingConfig();
		if (!pending) {
			result.status = SyncConfigReviewStatus::StoreError;
			result.storeStatus = pending.status;
			return result;
		}
		staged = pending.staged;
	}
	return ReviewSyncConfigInventory(inventory, state, staged, local);
}

SyncConfigPublishEntry PlanSyncConfigPublishEntry(
		const SyncConfigPublishRequest &request,
		bool staged) {
	const auto expects = request.expectedFingerprint.has_value()
		|| request.expectedParents.has_value();
	if (request.pendingOnly) {
		return (staged && !expects)
			? SyncConfigPublishEntry::FinishStaged
			: SyncConfigPublishEntry::Refuse;
	}
	return (!staged
			&& request.expectedFingerprint
			&& request.expectedParents)
		? SyncConfigPublishEntry::NewContent
		: SyncConfigPublishEntry::Refuse;
}

SyncConfigPublishGate PlanSyncConfigPublishGate(
		const SyncLocalState &state,
		const SyncAccountInventoryResult &inventory,
		const SyncOwnInventoryResult &own,
		const QString &localFingerprint,
		const SyncConfigPublishRequest &request) {
	auto result = SyncConfigPublishGate();
	const auto entry = PlanSyncConfigPublishEntry(
		request,
		state.config.pendingSeq != 0);
	if (entry != SyncConfigPublishEntry::NewContent
		|| *request.expectedFingerprint != localFingerprint) {
		return result;
	}
	const auto heads = ExtractSyncConfigHeads(
		inventory,
		state.space,
		state.install);
	const auto ownHead = FindSyncConfigOwnHead(inventory, own);
	if (heads.status != SyncConfigHeadsStatus::Complete
		|| AnyNewerSchema(heads.heads)
		|| !ownHead.valid) {
		return result;
	}
	const auto config = SyncConfigStateOf(state);
	result.plan = PlanConfigSync(
		localFingerprint,
		config,
		SyncConfigHeadsOf(heads.heads),
		ownHead.head
			? std::make_optional(ownHead.head->head)
			: std::nullopt);
	switch (result.plan.verdict) {
	case ConfigSyncVerdict::UpToDate:
		result.status = SyncConfigPublishGateStatus::AlreadySynced;
		return result;
	case ConfigSyncVerdict::Empty:
	case ConfigSyncVerdict::LocalChanges:
	case ConfigSyncVerdict::Choose:
	case ConfigSyncVerdict::Conflict:
		break;
	default:
		return result;
	}
	const auto choice = PlanConfigChoice(config, result.plan, std::nullopt);
	if (!choice
		|| !choice->publish
		|| !choice->adopt.empty()
		|| !SameSyncConfigKeySet(
			SyncConfigVersionKeys(choice->parents),
			*request.expectedParents)) {
		return result;
	}
	result.status = SyncConfigPublishGateStatus::Proceed;
	result.parents = choice->parents;
	return result;
}

} // namespace Purple

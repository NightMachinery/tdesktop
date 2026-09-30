/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_config_apply.h"

#include "purple/purple_config.h"
#include "purple/purple_state.h"
#include "purple/purple_sync_account_binding.h"

#include <QtCore/QDateTime>

#include <algorithm>

namespace Purple {
namespace {

constexpr auto kMaximumLabelLength = 256;

struct VerifiedWrite {
	SyncConfigApplyStatus status = SyncConfigApplyStatus::WriteError;
	QString historyId;
	SyncSettingsFile readBack;
	bool wrote = false;
};

[[nodiscard]] std::vector<QString> HeadKeys(
		const std::vector<ConfigHead> &heads) {
	auto result = std::vector<QString>();
	result.reserve(heads.size());
	for (const auto &head : heads) {
		result.push_back(head.key);
	}
	return result;
}

[[nodiscard]] bool SameHeadRecords(
		const std::vector<SyncConfigHeadRecord> &a,
		const std::vector<SyncConfigHeadRecord> &b) {
	return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](
			const SyncConfigHeadRecord &first,
			const SyncConfigHeadRecord &second) {
		return first.messageId == second.messageId
			&& first.head.space == second.head.space
			&& first.head.install == second.head.install
			&& first.head.seq == second.head.seq
			&& first.head.key == second.head.key
			&& first.head.lineage == second.head.lineage
			&& first.text == second.text;
	});
}

[[nodiscard]] bool SamePlan(
		const ConfigSyncPlan &a,
		const ConfigSyncPlan &b) {
	return a.verdict == b.verdict
		&& a.ownStale == b.ownStale
		&& HeadKeys(a.offered) == HeadKeys(b.offered)
		&& HeadKeys(a.same) == HeadKeys(b.same);
}

[[nodiscard]] QString KeyFingerprint(const QString &key) {
	const auto parsed = ParseConfigVersionKey(key);
	return parsed ? parsed->fingerprint : QString();
}

[[nodiscard]] QString SourceLabel(
		const SyncConfigHeadRecord &record,
		SyncConfigHistoryReason reason) {
	const auto device = SyncDeviceName(record);
	return ((reason == SyncConfigHistoryReason::BeforeUpdate)
		? u"Before update from %1"_q.arg(device)
		: u"Before using settings from %1"_q.arg(device)
	).left(kMaximumLabelLength);
}

[[nodiscard]] QString HistoryLabel(
		const SyncConfigHistoryEntry &entry,
		SyncConfigHistoryReason reason) {
	const auto when = SyncMomentText(
		QDateTime::fromMSecsSinceEpoch(entry.createdMs));
	return ((reason == SyncConfigHistoryReason::BeforeUndo)
		? u"Before undo to the version from %1"_q.arg(when)
		: u"Before restoring the version from %1"_q.arg(when)
	).left(kMaximumLabelLength);
}

[[nodiscard]] VerifiedWrite WriteSettingsWithHistory(
		const SyncSettingsFile &current,
		const QByteArray &text,
		const QString &fingerprint,
		SyncConfigHistoryReason reason,
		const QString &label,
		const QString &versionKey,
		const QString &keepId = QString()) {
	auto result = VerifiedWrite();
	if (SettingsFingerprint(text) != fingerprint
		|| !SyncSettingsTextWritable(text)) {
		return result;
	}
	const auto saved = SaveSyncConfigHistory(
		(current.status == SyncSettingsFileStatus::Present
			? std::make_optional(current.text)
			: std::nullopt),
		reason,
		label,
		versionKey,
		keepId);
	if (!saved) {
		result.status = SyncConfigApplyStatus::HistoryError;
		return result;
	}
	result.historyId = saved->id;
	if (!WriteConfigFile(SettingsFilePath(), QString::fromUtf8(text))) {
		return result;
	}
	result.wrote = true;
	result.readBack = ReadSyncSettingsFile(SettingsFilePath());
	if (result.readBack.status != SyncSettingsFileStatus::Present
		|| result.readBack.text != text
		|| result.readBack.fingerprint != fingerprint) {
		LOG(("Purple Error: settings.toml did not read back as written."));
		return result;
	}
	result.status = SyncConfigApplyStatus::Applied;
	return result;
}

[[nodiscard]] SyncConfigApplyResult Stop(
		SyncConfigApplyResult result,
		SyncConfigApplyStatus status,
		std::optional<SyncStoreStatus> storeStatus = std::nullopt) {
	result.status = status;
	result.storeStatus = storeStatus;
	return result;
}

void ProposeNextPublish(
		SyncConfigApplyResult &result,
		const ConfigSyncState &state,
		const std::vector<ConfigHead> &heads,
		const std::optional<ConfigHead> &ownHead) {
	const auto fresh = PlanConfigSync(
		result.fingerprint,
		state,
		heads,
		ownHead);
	result.nextVerdict = fresh.verdict;
	if (fresh.verdict != ConfigSyncVerdict::Empty
		&& fresh.verdict != ConfigSyncVerdict::LocalChanges
		&& fresh.verdict != ConfigSyncVerdict::Choose
		&& fresh.verdict != ConfigSyncVerdict::Conflict) {
		return;
	}
	const auto proposal = PlanConfigChoice(state, fresh, std::nullopt);
	if (proposal && proposal->publish && proposal->adopt.empty()) {
		result.publishNeeded = true;
		result.expectedParents = SyncConfigVersionKeys(proposal->parents);
	}
}

} // namespace

bool SyncSettingsTextWritable(const QByteArray &bytes) {
	return QString::fromUtf8(bytes).toUtf8() == bytes;
}

SyncConfigApplyResult ApplySyncConfigChoice(
		Main::Account &account,
		Main::Session &session,
		const SyncAccountInventoryResult &inventory,
		const SyncConfigReview &review,
		const std::optional<QString> &chosenRemoteKey) {
	auto result = SyncConfigApplyResult();
	if (!SyncAccountAvailable(account, session, inventory.accountUserId)
		|| review.accountUserId != inventory.accountUserId) {
		return Stop(result, SyncConfigApplyStatus::AccountUnavailable);
	} else if (review.status != SyncConfigReviewStatus::Ready
		|| review.local.status == SyncSettingsFileStatus::Invalid
		|| (!review.bound && !review.state.install.isEmpty())) {
		return Stop(result, SyncConfigApplyStatus::NeedsReview);
	} else if (!PlanConfigChoice(review.state, review.plan, chosenRemoteKey)) {
		return Stop(result, SyncConfigApplyStatus::InvalidChoice);
	}
	if (review.space.isEmpty()) {
		if (review.bound
			|| inventory.directory.selectedSpace
			|| !inventory.directory.canCreateSpace) {
			return Stop(result, SyncConfigApplyStatus::NeedsRecheck);
		}
	} else {
		const auto current = ExtractSyncConfigHeads(
			inventory,
			review.space,
			review.state.install);
		if (current.status != SyncConfigHeadsStatus::Complete
			|| !SameHeadRecords(current.heads, review.heads)) {
			return Stop(result, SyncConfigApplyStatus::NeedsRecheck);
		}
	}
	if (!SameSyncSettingsFile(
			ReadSyncSettingsFile(SettingsFilePath()),
			review.local)) {
		return Stop(result, SyncConfigApplyStatus::NeedsRecheck);
	}

	if (!review.bound) {
		const auto setup = InitializeSyncAccountLocally(
			account,
			session,
			inventory);
		result.setupStatus = setup.status;
		if (setup.status == SyncAccountSetupStatus::AlreadyBound) {
			return Stop(result, SyncConfigApplyStatus::NeedsRecheck);
		} else if (setup.status != SyncAccountSetupStatus::Ready) {
			return Stop(
				result,
				SyncConfigApplyStatus::SetupFailed,
				setup.storeStatus);
		}
		result.joined = true;
	}

	auto store = SyncLocalStore(ConfigDirectory() + u"/sync"_q);
	const auto opened = store.Open(true);
	const auto state = store.state();
	if (opened.status != SyncStoreStatus::Ready || !state) {
		return Stop(
			result,
			SyncConfigApplyStatus::StoreError,
			(opened.status == SyncStoreStatus::Ready
				? SyncStoreStatus::InvalidState
				: opened.status));
	} else if (CheckAccountSyncBinding(*state, account)
			!= SyncAccountBindingVerdict::Bound) {
		return Stop(result, SyncConfigApplyStatus::AccountUnbound);
	} else if ((review.bound && state->install != review.state.install)
		|| (!review.space.isEmpty() && state->space != review.space)) {
		return Stop(result, SyncConfigApplyStatus::NeedsRecheck);
	}
	const auto token = AccountSyncBindingToken(account);
	const auto local = ReadSyncSettingsFile(SettingsFilePath());
	if (!SameSyncSettingsFile(local, review.local)) {
		return Stop(result, SyncConfigApplyStatus::NeedsRecheck);
	}
	const auto config = SyncConfigStateOf(*state);
	const auto heads = SyncConfigHeadsOf(review.heads);
	const auto ownHead = review.ownHead
		? std::make_optional(review.ownHead->head)
		: std::nullopt;
	const auto plan = PlanConfigSync(
		local.fingerprint,
		config,
		heads,
		ownHead);
	if (!SamePlan(plan, review.plan)) {
		return Stop(result, SyncConfigApplyStatus::NeedsRecheck);
	}
	const auto choice = PlanConfigChoice(config, plan, chosenRemoteKey);
	if (!choice) {
		return Stop(result, SyncConfigApplyStatus::InvalidChoice);
	}

	auto next = config;
	result.fingerprint = local.fingerprint;
	if (choice->writeRemote) {
		const auto record = std::find_if(
			review.heads.begin(),
			review.heads.end(),
			[&](const SyncConfigHeadRecord &record) {
				return record.head.key == choice->write.key
					&& record.head.install == choice->write.install
					&& record.head.seq == choice->write.seq;
			});
		if (record == review.heads.end()) {
			return Stop(result, SyncConfigApplyStatus::NeedsReview);
		}
		const auto baseKnown = !config.base.isEmpty()
			&& KeyFingerprint(config.base) == local.fingerprint;
		const auto reason = (plan.verdict == ConfigSyncVerdict::UpdateReady)
			? SyncConfigHistoryReason::BeforeUpdate
			: SyncConfigHistoryReason::BeforeChoice;
		const auto write = WriteSettingsWithHistory(
			local,
			record->text,
			KeyFingerprint(record->head.key),
			reason,
			SourceLabel(*record, reason),
			baseKnown ? config.base : QString());
		result.historyId = write.historyId;
		result.wroteFile = write.wrote;
		if (write.status != SyncConfigApplyStatus::Applied) {
			return Stop(result, write.status);
		}
		NoteSettingsImported(write.readBack.text);
		result.fingerprint = write.readBack.fingerprint;
	}
	if (!choice->adopt.empty()) {
		const auto adopted = AdoptConfigHeads(
			config,
			result.fingerprint,
			choice->adopt);
		if (!adopted) {
			return Stop(result, SyncConfigApplyStatus::NeedsReview);
		}
		const auto committed = store.CommitConfigData(
			SyncLocalConfigOf(*adopted),
			token);
		if (!committed) {
			return Stop(
				result,
				SyncConfigApplyStatus::StoreError,
				committed.status);
		}
		next = *adopted;
		result.adopted = true;
	}
	ProposeNextPublish(result, next, heads, ownHead);
	if (choice->writeRemote
		&& (choice->publish != result.publishNeeded
			|| !SameSyncConfigKeySet(
				SyncConfigVersionKeys(choice->parents),
				result.expectedParents))) {
		LOG(("Purple Error: the settings choice promised a different "
			"publish than a fresh check proposes."));
	}
	result.status = SyncConfigApplyStatus::Applied;
	return result;
}

SyncConfigRestoreResult RestoreSyncConfigHistory(
		const QString &id,
		SyncConfigHistoryReason reason) {
	auto result = SyncConfigRestoreResult();
	if (reason != SyncConfigHistoryReason::BeforeRestore
		&& reason != SyncConfigHistoryReason::BeforeUndo) {
		result.status = SyncConfigRestoreStatus::InvalidReason;
		return result;
	}
	const auto entries = ListSyncConfigHistory();
	const auto entry = std::find_if(
		entries.begin(),
		entries.end(),
		[&](const SyncConfigHistoryEntry &entry) { return entry.id == id; });
	if (entry == entries.end()) {
		return result;
	} else if (!entry->existed) {
		result.status = SyncConfigRestoreStatus::FileDidNotExist;
		return result;
	}
	const auto text = ReadSyncConfigHistory(id);
	if (!text || SettingsFingerprint(*text) != entry->fingerprint) {
		return result;
	} else if (!SyncSettingsTextWritable(*text)) {
		result.status = SyncConfigRestoreStatus::NotText;
		return result;
	}
	const auto local = ReadSyncSettingsFile(SettingsFilePath());
	if (local.status == SyncSettingsFileStatus::Invalid) {
		result.status = SyncConfigRestoreStatus::InvalidSettings;
		return result;
	} else if (local.status == SyncSettingsFileStatus::Present
		&& local.text == *text) {
		result.status = SyncConfigRestoreStatus::Unchanged;
		result.fingerprint = local.fingerprint;
		return result;
	}
	const auto write = WriteSettingsWithHistory(
		local,
		*text,
		entry->fingerprint,
		reason,
		HistoryLabel(*entry, reason),
		QString(),
		id);
	result.historyId = write.historyId;
	switch (write.status) {
	case SyncConfigApplyStatus::Applied:
		result.status = SyncConfigRestoreStatus::Restored;
		result.fingerprint = write.readBack.fingerprint;
		break;
	case SyncConfigApplyStatus::HistoryError:
		result.status = SyncConfigRestoreStatus::HistoryError;
		break;
	default:
		result.status = SyncConfigRestoreStatus::WriteError;
		break;
	}
	return result;
}

} // namespace Purple

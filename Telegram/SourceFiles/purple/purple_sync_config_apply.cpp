/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_config_apply.h"

#include "purple/purple_config.h"
#include "main/main_session.h"
#include "purple/purple_state.h"
#include "purple/purple_sync_account_binding.h"
#include "purple/purple_sync_account_recovery.h"
#include "purple/purple_sync_config_text.h"

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
	bool restorable = false;
};

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
		Main::Session &session,
		const AccountOperationTarget &target,
		const SyncSettingsFile &current,
		const QByteArray &text,
		const QString &fingerprint,
		SyncConfigHistoryReason reason,
		const QString &label,
		const QString &versionKey,
		const QString &keepId = QString(),
		const AccountTransferReview *replacement = nullptr) {
	auto result = VerifiedWrite();
	const auto snapshot = (current.status == SyncSettingsFileStatus::Present)
		? SettingsFileInput{ SettingsFileState::Readable, current.text }
		: SettingsFileInput{ SettingsFileState::Missing };
	if (current.status == SyncSettingsFileStatus::Invalid
		|| snapshot != target.file.input
		|| !ValidateAccountSyncTarget(&session, target)
		|| !replacement || replacement->impact.proposedText != text
		|| !RebaseAccountTransferAfterSetup(&session, *replacement)
		|| SettingsFingerprint(text) != fingerprint
		|| !SyncSettingsTextWritable(text)) {
		return result;
	}
	const auto saved = SaveSyncConfigHistory(
		AccountSyncHistoryAccess(&session, target),
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
	result.restorable = saved->existed
		&& SyncSettingsTextWritable(current.text);
	const auto ready = RebaseAccountTransferAfterSetup(&session, *replacement);
	if (!ready) { return result; }
	const auto written = ReplaceReviewedAccountSettings(&session, *ready);
	result.wrote = written.changed;
	if (!written.ok) { return result; }
	result.wrote = true;
	result.readBack = ReadSyncSettingsFile(target.files);
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

[[nodiscard]] SyncConfigApplyStatus StatusOf(
		SyncConfigApplyPlanStatus status) {
	switch (status) {
	case SyncConfigApplyPlanStatus::Ready:
		return SyncConfigApplyStatus::Applied;
	case SyncConfigApplyPlanStatus::NeedsRecheck:
		return SyncConfigApplyStatus::NeedsRecheck;
	case SyncConfigApplyPlanStatus::NeedsReview:
		return SyncConfigApplyStatus::NeedsReview;
	case SyncConfigApplyPlanStatus::InvalidChoice:
		return SyncConfigApplyStatus::InvalidChoice;
	}
	return SyncConfigApplyStatus::NeedsReview;
}

} // namespace

SyncConfigApplyResult ApplySyncConfigChoice(
		Main::Account &account,
		Main::Session &session,
		const SyncAccountInventoryResult &inventory,
		const SyncConfigReview &review,
		const std::optional<QString> &chosenRemoteKey,
		const AccountTransferReview *replacement,
		SyncLocalStore *lockedStore) {
	auto result = SyncConfigApplyResult();
	const auto recovery = InspectSyncAccountRecovery(session, &inventory);
	if (!recovery || recovery->journalPresent || recovery->plan.status != (review.bound
		? SyncAccountRecoveryStatus::Bound : SyncAccountRecoveryStatus::Setup)) {
		return Stop(result, SyncConfigApplyStatus::NeedsReview);
	}
	auto target = AccountSyncTarget(&session);
	if (!target) { return Stop(result, SyncConfigApplyStatus::AccountUnavailable); }
	if (!SyncAccountAvailable(account, session, inventory.accountUserId)
		|| review.accountUserId != inventory.accountUserId) {
		return Stop(result, SyncConfigApplyStatus::AccountUnavailable);
	}
	if (!replacement && (review.plan.verdict == ConfigSyncVerdict::Choose
		|| review.plan.verdict == ConfigSyncVerdict::Conflict
		|| review.plan.verdict == ConfigSyncVerdict::UpdateReady)) {
		return Stop(result, SyncConfigApplyStatus::NeedsReview);
	}
	if (replacement) {
		const auto choices = SyncConfigChoices(review, SyncSettingsPublishable(review), replacement->context);
		const auto choice = std::find_if(choices.begin(), choices.end(), [&](const auto &value) {
			return value.key == chosenRemoteKey;
		});
		if (choice == choices.end() || !choice->impactReady
			|| !RebaseAccountTransferAfterSetup(&session, *replacement)) {
			return Stop(result, SyncConfigApplyStatus::NeedsRecheck);
		}
	}
	const auto checked = CheckSyncConfigApplyChoice(review, chosenRemoteKey);
	if (checked != SyncConfigApplyPlanStatus::Ready) {
		return Stop(result, StatusOf(checked));
	}
	const auto stamp = SyncConfigReviewStamp(review);
	if (!review.bound) {
		const auto before = PlanSyncConfigApply(
			ReviewSyncConfigInventory(
				inventory,
				nullptr,
				{},
				ReadSyncSettingsFile(target->files)),
			stamp,
			chosenRemoteKey);
		if (before.status != SyncConfigApplyPlanStatus::Ready) {
			return Stop(result, StatusOf(before.status));
		}
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
		target = AccountSyncTarget(&session);
		if (!target) { return Stop(result, SyncConfigApplyStatus::AccountUnavailable); }
	}

	if (lockedStore && lockedStore->directory() != target->files.syncDirectory) {
		return Stop(result, SyncConfigApplyStatus::NeedsRecheck);
	}
	auto ownedStore = lockedStore ? std::unique_ptr<SyncLocalStore>()
		: std::make_unique<SyncLocalStore>(target->files.syncDirectory);
	auto &store = lockedStore ? *lockedStore : *ownedStore;
	const auto opened = lockedStore ? SyncStoreResult{ store.status() } : store.Open(true);
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
	}
	auto staged = QByteArray();
	if (state->config.pendingSeq) {
		const auto pending = store.ReadPendingConfig();
		if (!pending) {
			return Stop(
				result,
				SyncConfigApplyStatus::StoreError,
				pending.status);
		}
		staged = pending.staged;
	}
	const auto token = AccountSyncBindingToken(account);
	const auto expectedStore = ReadSettingsFileInput(target->files.syncStore);
	const auto local = ReadSyncSettingsFile(target->files);
	const auto plan = PlanSyncConfigApply(
		ReviewSyncConfigInventory(inventory, state, staged, local),
		(review.bound
			? stamp
			: SyncConfigReviewStamp(SyncConfigJoinedReview(review, *state))),
		chosenRemoteKey);
	if (plan.status != SyncConfigApplyPlanStatus::Ready || plan.join) {
		return Stop(
			result,
			(plan.join
				? SyncConfigApplyStatus::NeedsRecheck
				: StatusOf(plan.status)));
	}

	auto current = local;
	result.fingerprint = local.fingerprint;
	if (plan.choice.writeRemote) {
		result.otherVersionsRemain = plan.otherVersionsRemain;
		const auto reason = plan.update
			? SyncConfigHistoryReason::BeforeUpdate
			: SyncConfigHistoryReason::BeforeChoice;
		const auto write = WriteSettingsWithHistory(
			session, *target,
			local,
			plan.source->text,
			plan.writeFingerprint,
			reason,
			SourceLabel(*plan.source, reason),
			plan.versionKey,
			QString(),
			replacement);
		result.historyId = write.historyId;
		result.wroteFile = write.wrote;
		result.undoAvailable = write.wrote && write.restorable;
		if (write.status != SyncConfigApplyStatus::Applied) {
			return Stop(result, write.status);
		}
		NoteSettingsImported(&session, write.readBack.text);
		result.fingerprint = write.readBack.fingerprint;
		current = write.readBack;
	}
	const auto completion = CompleteSyncConfigApply(plan, current);
	switch (completion.status) {
	case SyncConfigApplyCompletionStatus::Ready:
		break;
	case SyncConfigApplyCompletionStatus::ReadBackMismatch:
		return Stop(result, SyncConfigApplyStatus::WriteError);
	case SyncConfigApplyCompletionStatus::InvalidPlan:
	case SyncConfigApplyCompletionStatus::AdoptRefused:
		return Stop(result, SyncConfigApplyStatus::NeedsReview);
	}
	auto completedTarget = AccountSyncTarget(&session).value_or(*target);
	if (completedTarget.file.account != target->file.account
		|| completedTarget.file.path != target->file.path
		|| completedTarget.file.generation != target->file.generation) {
		return Stop(result, SyncConfigApplyStatus::NeedsRecheck);
	}
	completedTarget.file.input = { SettingsFileState::Readable, current.text };
	if (!ValidateAccountSyncTarget(&session, completedTarget)
		|| AccountSyncBindingToken(account) != token
		|| !ValidateAccountSyncStoreTarget(&session, completedTarget, expectedStore)) {
		return Stop(result, SyncConfigApplyStatus::NeedsRecheck);
	}
	if (completion.adopted) {
		const auto committed = store.CommitConfigData(
			SyncLocalConfigOf(*completion.adopted),
			token);
		if (!committed) {
			return Stop(
				result,
				SyncConfigApplyStatus::StoreError,
				committed.status);
		}
		result.adopted = true;
	}
	result.nextVerdict = completion.nextVerdict;
	result.publishNeeded = completion.publishNeeded;
	result.expectedParents = completion.expectedParents;
	if (!completion.promiseKept) {
		LOG(("Purple Error: the settings choice promised a different "
			"publish than a fresh check proposes."));
	}
	result.status = SyncConfigApplyStatus::Applied;
	return result;
}

SyncConfigRestoreResult RestoreSyncConfigHistory(
		Main::Session &session,
		const QString &id,
		SyncConfigHistoryReason reason,
		const AccountTransferReview &replacement) {
	const auto target = AccountSyncInspectionTarget(&session);
	if (!target) {
		return { .status = SyncConfigRestoreStatus::InvalidSettings };
	}

	auto result = SyncConfigRestoreResult();
	if (reason != SyncConfigHistoryReason::BeforeRestore
		&& reason != SyncConfigHistoryReason::BeforeUndo) {
		result.status = SyncConfigRestoreStatus::InvalidReason;
		return result;
	}
	const auto entries = ListSyncConfigHistory(AccountSyncHistoryAccess(&session, *target));
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
	const auto text = ReadSyncConfigHistory(AccountSyncHistoryAccess(&session, *target), id);
	if (!text || SettingsFingerprint(*text) != entry->fingerprint) {
		return result;
	} else if (!SyncSettingsTextWritable(*text)) {
		result.status = SyncConfigRestoreStatus::NotText;
		return result;
	}
	const auto local = ReadSyncSettingsFile(target->files);
	if (local.status == SyncSettingsFileStatus::Invalid
		|| !RebaseAccountTransferAfterSetup(&session, replacement)) {
		result.status = SyncConfigRestoreStatus::InvalidSettings;
		return result;
	} else if (local.status == SyncSettingsFileStatus::Present
		&& local.text == *text) {
		result.status = SyncConfigRestoreStatus::Unchanged;
		result.fingerprint = local.fingerprint;
		return result;
	}
	const auto write = WriteSettingsWithHistory(
		session, *target,
		local,
		*text,
		entry->fingerprint,
		reason,
		HistoryLabel(*entry, reason),
		QString(),
		id,
		&replacement);
	result.historyId = write.historyId;
	switch (write.status) {
	case SyncConfigApplyStatus::Applied:
		result.status = SyncConfigRestoreStatus::Restored;
		result.fingerprint = write.readBack.fingerprint;
		NoteSettingsImported(&session, write.readBack.text);
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

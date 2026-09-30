/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_config_text.h"

#include "purple/purple_sync_envelope.h"

#include <QtCore/QStringList>

#include <algorithm>

namespace Purple {
namespace {

[[nodiscard]] QString Plural(
		int count,
		const QString &one,
		const QString &many) {
	return (count == 1) ? one : many.arg(count);
}

[[nodiscard]] std::vector<QString> DeviceNames(
		const std::vector<SyncDeviceNameParts> &devices) {
	auto result = std::vector<QString>();
	for (const auto &device : devices) {
		const auto name = SyncDeviceName(device);
		if (std::find(result.begin(), result.end(), name) == result.end()) {
			result.push_back(name);
		}
	}
	return result;
}

[[nodiscard]] QString ChooseText(const SyncConfigReview &review) {
	const auto devices = SyncDeviceList(SyncOfferedDeviceNames(review));
	switch (SyncConfigChooseMessage(review)) {
	case SyncConfigMessage::ChooseBound:
		return u"Settings from %1 are not related to this device's."_q.arg(
			devices);
	case SyncConfigMessage::ChooseUnbound:
		return u"Settings from %1 are already in this account, and this "
			"device's settings are different."_q.arg(devices);
	case SyncConfigMessage::ConflictConcurrent:
		return u"This device and %1 both changed settings since they last "
			"matched."_q.arg(devices);
	case SyncConfigMessage::ConflictSplitBound:
		return u"Settings from %1 differ from each other."_q.arg(devices);
	default:
		break;
	}
	return u"Settings from %1 differ from each other and from this "
		"device's."_q.arg(devices);
}

[[nodiscard]] QString JoinedFailureReason(
		const SyncConfigApplyFailure &failure) {
	switch (failure.status) {
	case SyncConfigApplyStatus::NeedsRecheck:
		return u"settings or Saved Messages changed since the check"_q;
	case SyncConfigApplyStatus::NeedsReview:
	case SyncConfigApplyStatus::InvalidChoice:
		return u"that choice is no longer available"_q;
	case SyncConfigApplyStatus::AccountUnavailable:
		return u"the selected account became unavailable"_q;
	case SyncConfigApplyStatus::AccountUnbound:
		return u"the link to this account could not be confirmed"_q;
	case SyncConfigApplyStatus::StoreError:
		return u"this device's sync state could not be opened or saved"_q;
	case SyncConfigApplyStatus::InvalidSettings:
		return u"settings.toml could not be read"_q;
	case SyncConfigApplyStatus::HistoryError:
		return u"a copy of the current settings could not be kept in "
			"History"_q;
	case SyncConfigApplyStatus::WriteError:
		return failure.historyKept
			? u"writing the file failed"_q
			: u"the chosen version could not be written exactly"_q;
	case SyncConfigApplyStatus::Applied:
	case SyncConfigApplyStatus::SetupFailed:
		break;
	}
	return u"the choice could not be finished"_q;
}

[[nodiscard]] QString WrittenFailureText(
		const SyncConfigApplyFailure &failure) {
	const auto undo = failure.undoAvailable
		? u" Undo puts the previous file back."_q
		: QString();
	if (failure.kind == SyncConfigApplyFailureKind::WrittenStateNotSaved) {
		return u"settings.toml was updated, but this device's sync state "
			"could not be saved. "_q
			+ (failure.otherVersionsRemain
				? u"The next check asks you to choose again, with this "
					"version as this device's settings."_q
				: u"The next check records the update without changing the "
					"file."_q)
			+ undo;
	}
	return u"settings.toml was written but did not read back as expected. "
		"The previous file is in History. Check again."_q + undo;
}

} // namespace

QString SyncCloudDisclosureText() {
	return u"The settings file may contain chat IDs and names. Saved "
		"Messages is a Telegram cloud chat: it is not end-to-end encrypted, "
		"and every signed-in session can read it. state.toml stays on this "
		"device."_q;
}

QString SyncDeviceList(std::vector<QString> names) {
	names.erase(
		std::remove_if(names.begin(), names.end(), [](const QString &name) {
			return name.isEmpty();
		}),
		names.end());
	if (names.empty()) {
		return u"another device"_q;
	} else if (names.size() == 1) {
		return names.front();
	}
	const auto last = names.back();
	names.pop_back();
	return QStringList(names.begin(), names.end()).join(u", "_q)
		+ u" and "_q
		+ last;
}

QString SyncRecordTimeText(uint64_t seconds) {
	return SyncMomentText(QDateTime::fromSecsSinceEpoch(qint64(seconds)));
}

std::vector<QString> SyncOfferedDeviceNames(const SyncConfigReview &review) {
	return DeviceNames(SyncConfigDeviceNames(review, review.plan.offered));
}

bool SyncSettingsPublishable(const SyncSettingsFile &file) {
	const auto install = FormatSyncInstallId(QByteArray(16, '\0'));
	return install
		&& SyncSettingsPublishable(
			file,
			u"desktop:"_q + *install,
			SyncWriter());
}

SyncConfigBoxStatus DescribeSyncConfigReview(
		const SyncConfigReview &review,
		const QDateTime &checkedAt) {
	using Message = SyncConfigMessage;
	const auto described = DescribeSyncConfigReview(
		review,
		SyncSettingsPublishable(review.local));
	const auto action = described.action;
	switch (described.message) {
	case Message::NeedsReviewWithPending:
		return { u"A settings post from this device may not have finished, "
			"but Saved Messages also has sync records this device cannot "
			"safely use: they are ambiguous, unreadable, or in another sync "
			"space. The earlier post stays paused while they remain. Nothing "
			"was sent."_q };
	case Message::NeedsReview:
		return { u"Saved Messages has sync records this device cannot "
			"safely use: they are ambiguous, unreadable, or in another sync "
			"space. Nothing was changed."_q };
	case Message::Incomplete:
		return { u"The check did not finish, so nothing can be decided yet. "
			"Check again."_q };
	case Message::CloneDetected:
		return { u"Another device is using this device's sync identity, so "
			"sync is paused here. Nothing was changed."_q };
	case Message::AccountUnavailable:
		return { u"The selected account is unavailable. Choose an account and "
			"check again."_q };
	case Message::AccountUnbound:
		return { u"This device's sync state belongs to another account, or "
			"its link to this account was lost. Nothing was changed."_q };
	case Message::StoreError:
		return { u"This device's sync state could not be opened. Nothing was "
			"changed."_q };
	case Message::InvalidSettings:
		return { u"settings.toml is not a regular file or is larger than "
			"256 KB, so sync cannot use it."_q };
	case Message::InvalidRecords:
		return { u"A sync record in Saved Messages or this device's sync "
			"state is not valid, so nothing was changed. Check again later; "
			"if this stays, the records need manual attention."_q };
	case Message::Pending:
		return {
			u"A settings post may not have finished. Finish sending checks "
				"Saved Messages again and completes it without sending "
				"twice."_q,
			action,
		};
	case Message::ChooseBound:
	case Message::ChooseUnbound:
	case Message::ConflictConcurrent:
	case Message::ConflictSplitBound:
	case Message::ConflictSplitUnbound:
		return {
			ChooseText(review) + u" Choose which settings to use."_q,
			action,
		};
	case Message::UpdateReady:
		return {
			u"Update ready from %1, %2."_q.arg(
				SyncDeviceName(described.devices.front()),
				SyncRecordTimeText(described.at)),
			action,
		};
	case Message::UpdateMissing:
		return { u"An update is ready, but its record could not be found. "
			"Check again."_q };
	case Message::AdoptUnbound:
		return {
			u"%1 already has exactly these settings. Join sync to keep this "
				"device in step with it."_q.arg(
					SyncDeviceList(DeviceNames(described.devices))),
			action,
		};
	case Message::AdoptBound:
		return { u"%1 has exactly these settings."_q.arg(
			SyncDeviceList(DeviceNames(described.devices))) };
	case Message::NotPublishableAbsent:
		return { u"settings.toml does not exist, so there is nothing to "
			"send."_q };
	case Message::NotPublishableInvalid:
		return { u"settings.toml is empty, is not valid TOML, or is too large "
			"to send. Fix it and check again."_q };
	case Message::EmptyBound:
		return {
			u"No settings are in this sync space yet. Publish settings sends "
				"this device's settings."_q,
			action,
		};
	case Message::EmptyUnbound:
		return {
			u"No sync records yet. Publish settings starts sync with this "
				"device's settings."_q,
			action,
		};
	case Message::LocalChangesEdited:
		return { u"This device has changes that are not synced."_q, action };
	case Message::LocalChangesOwnStale:
		return {
			u"Your other devices have not seen this device's current "
				"settings yet."_q,
			action,
		};
	case Message::UpToDateAlone:
	case Message::UpToDateWith: {
		const auto checked = checkedAt.toLocalTime().toString(u"HH:mm"_q);
		return { (described.message == Message::UpToDateAlone)
			? u"Up to date. No other devices sync yet. Checked %1."_q.arg(
				checked)
			: u"Up to date with %1. Checked %2."_q.arg(
				Plural(
					described.others,
					u"1 other device"_q,
					u"%1 other devices"_q),
				checked) };
	}
	}
	return {};
}

QString SyncConfigBoxActionText(SyncConfigBoxAction action) {
	switch (action) {
	case SyncConfigBoxAction::None: return QString();
	case SyncConfigBoxAction::Publish: return u"Publish settings"_q;
	case SyncConfigBoxAction::Join: return u"Join sync"_q;
	case SyncConfigBoxAction::ReviewUpdate: return u"Review update"_q;
	case SyncConfigBoxAction::Choose: return u"Choose settings"_q;
	case SyncConfigBoxAction::PublishChanges: return u"Publish changes"_q;
	case SyncConfigBoxAction::FinishSending: return u"Finish sending"_q;
	}
	return QString();
}

QString SyncConfigChoiceIntro(const SyncConfigReview &review) {
	const auto &plan = review.plan;
	if (plan.verdict == ConfigSyncVerdict::UpdateReady) {
		const auto record = plan.offered.empty()
			? nullptr
			: FindSyncConfigHeadRecord(review, plan.offered.front());
		const auto device = record
			? SyncDeviceName(*record)
			: u"another device"_q;
		return u"%1 changed settings. Applying replaces this device's "
			"settings.toml with that version and keeps the current file in "
			"History. Nothing is sent."_q.arg(device);
	}
	return ChooseText(review)
		+ u" Nothing changes until you choose. If you pick another "
			"device's version, this device's current file is kept in "
			"History; the other devices keep their own records."_q;
}

QString SyncConfigChoiceButtonText(
		const SyncConfigReview &review,
		const std::optional<QString> &chosenRemoteKey) {
	if (review.plan.verdict == ConfigSyncVerdict::UpdateReady) {
		return u"Apply"_q;
	}
	const auto publishes = SyncChoicePublishes(review, chosenRemoteKey);
	return review.bound
		? (publishes ? u"Use and share"_q : u"Use this version"_q)
		: (publishes ? u"Join and share"_q : u"Join with this version"_q);
}

std::vector<QString> SyncConfigChangeLines(
		const ConfigChangeSummary &summary,
		const ConfigTextDiff &diff) {
	if (diff.identical) {
		return { u"No line changes."_q };
	} else if (!summary.parsed) {
		return {
			u"%1, %2. One side is not valid TOML, so only line counts are "
				"shown."_q.arg(
					Plural(
						diff.added,
						u"1 line added"_q,
						u"%1 lines added"_q),
					Plural(
						diff.removed,
						u"1 line removed"_q,
						u"%1 lines removed"_q)),
		};
	} else if (summary.entries.empty()) {
		return { u"No setting changes: only comments or formatting "
			"differ."_q };
	}
	const auto shown = std::min(
		int(summary.entries.size()),
		kSyncConfigSummaryLimit);
	auto result = std::vector<QString>();
	for (const auto kind : {
		ConfigChangeKind::Changed,
		ConfigChangeKind::Added,
		ConfigChangeKind::Removed,
	}) {
		auto labels = QStringList();
		for (auto i = 0; i != shown; ++i) {
			if (summary.entries[i].kind == kind) {
				labels.push_back(summary.entries[i].label);
			}
		}
		if (labels.isEmpty()) {
			continue;
		}
		const auto prefix = (kind == ConfigChangeKind::Changed)
			? u"Changed: "_q
			: (kind == ConfigChangeKind::Added)
			? u"Added: "_q
			: u"Removed: "_q;
		result.push_back(prefix + labels.join(u", "_q));
	}
	const auto more = int(summary.entries.size()) - shown;
	if (more > 0) {
		result.push_back(u"and %1 more"_q.arg(more));
	}
	return result;
}

QString SyncConfigDiffText(const ConfigTextDiff &diff, int lineLimit) {
	if (diff.identical) {
		return u"No line changes."_q;
	}
	auto lines = QStringList();
	if (diff.truncated) {
		lines.push_back(u"The files differ in more than %1 lines, so the "
			"whole file is shown as replaced."_q.arg(kConfigDiffEditLimit));
	}
	auto total = 0;
	auto shown = 0;
	for (const auto &hunk : diff.hunks) {
		total += 1 + int(hunk.lines.size());
	}
	for (const auto &hunk : diff.hunks) {
		if (shown >= lineLimit) {
			break;
		}
		lines.push_back(u"@@ -%1,%2 +%3,%4 @@"_q
			.arg(hunk.oldStart)
			.arg(hunk.oldCount)
			.arg(hunk.newStart)
			.arg(hunk.newCount));
		++shown;
		for (const auto &line : hunk.lines) {
			if (shown >= lineLimit) {
				break;
			}
			const auto prefix = (line.kind == ConfigDiffLineKind::Added)
				? u'+'
				: (line.kind == ConfigDiffLineKind::Removed)
				? u'-'
				: u' ';
			lines.push_back(prefix + line.text);
			++shown;
		}
	}
	if (total > shown) {
		lines.push_back(Plural(
			total - shown,
			u"... 1 more line not shown."_q,
			u"... %1 more lines not shown."_q));
	}
	return lines.join(u'\n');
}

QString SyncConfigHistoryMomentText(const SyncConfigHistoryEntry &entry) {
	return SyncMomentText(QDateTime::fromMSecsSinceEpoch(entry.createdMs));
}

QString SyncConfigHistoryRowText(const SyncConfigHistoryEntry &entry) {
	const auto label = !entry.label.isEmpty()
		? entry.label
		: (entry.reason == SyncConfigHistoryReason::BeforeUpdate)
		? u"Before an update"_q
		: (entry.reason == SyncConfigHistoryReason::BeforeChoice)
		? u"Before a choice"_q
		: (entry.reason == SyncConfigHistoryReason::BeforeRestore)
		? u"Before a restore"_q
		: u"Before an undo"_q;
	return SyncConfigHistoryMomentText(entry)
		+ u" · "_q
		+ label
		+ (entry.existed ? QString() : u" · no file"_q);
}

QString SyncConfigApplyFailureText(const SyncConfigApplyResult &result) {
	const auto failure = DescribeSyncConfigApplyFailure({
		.status = result.status,
		.joined = result.joined,
		.wroteFile = result.wroteFile,
		.historyKept = !result.historyId.isEmpty(),
		.undoAvailable = result.undoAvailable,
		.otherVersionsRemain = result.otherVersionsRemain,
	});
	switch (failure.kind) {
	case SyncConfigApplyFailureKind::None:
		return QString();
	case SyncConfigApplyFailureKind::JoinedNotWritten:
		return u"Joined sync, but %1, so nothing was written to "
			"settings.toml. Check again."_q.arg(JoinedFailureReason(failure));
	case SyncConfigApplyFailureKind::WrittenStateNotSaved:
	case SyncConfigApplyFailureKind::WrittenNotReadBack:
		return (failure.joined ? u"Joined sync. "_q : QString())
			+ WrittenFailureText(failure);
	case SyncConfigApplyFailureKind::NothingDone:
		break;
	}
	switch (failure.status) {
	case SyncConfigApplyStatus::Applied:
		return QString();
	case SyncConfigApplyStatus::NeedsRecheck:
		return u"Settings or Saved Messages changed since the check, so "
			"nothing was done. Check again."_q;
	case SyncConfigApplyStatus::NeedsReview:
	case SyncConfigApplyStatus::InvalidChoice:
		return u"That choice is no longer available, so nothing was done. "
			"Check again."_q;
	case SyncConfigApplyStatus::AccountUnavailable:
		return u"The selected account is unavailable, so nothing was done. "
			"Choose an account and check again."_q;
	case SyncConfigApplyStatus::AccountUnbound:
		return u"This device's sync state is not linked to this account, so "
			"nothing was done."_q;
	case SyncConfigApplyStatus::SetupFailed:
		return u"Could not set up sync on this device, so nothing was done. "
			"Check again."_q;
	case SyncConfigApplyStatus::StoreError:
		return u"This device's sync state could not be opened or saved, so "
			"nothing was done."_q;
	case SyncConfigApplyStatus::InvalidSettings:
		return u"settings.toml is not a regular file or is too large, so "
			"nothing was done."_q;
	case SyncConfigApplyStatus::HistoryError:
		return u"Could not keep a copy of the current settings in History, "
			"so nothing was changed."_q;
	case SyncConfigApplyStatus::WriteError:
		return failure.historyKept
			? u"Could not write settings.toml, so it was not changed. A copy "
				"of it was also kept in History."_q
			: u"The chosen version could not be written exactly, so nothing "
				"was changed."_q;
	}
	return QString();
}

bool SyncConfigUndoFinished(const SyncConfigRestoreResult &result) {
	return SyncConfigUndoFinished(result.status);
}

QString SyncConfigRestoreText(
		const SyncConfigRestoreResult &result,
		const SyncConfigHistoryEntry &entry) {
	const auto when = SyncConfigHistoryMomentText(entry);
	switch (result.status) {
	case SyncConfigRestoreStatus::Restored:
		return u"Restored the version from %1. The change stays on this "
			"device until you publish it."_q.arg(when);
	case SyncConfigRestoreStatus::Unchanged:
		return u"settings.toml already has the version from %1."_q.arg(when);
	case SyncConfigRestoreStatus::NotFound:
		return u"That History entry is gone or no longer valid. Nothing was "
			"changed."_q;
	case SyncConfigRestoreStatus::FileDidNotExist:
		return u"That entry records that settings.toml did not exist, so "
			"there is nothing to restore."_q;
	case SyncConfigRestoreStatus::NotText:
		return u"That copy is not valid UTF-8 text, so it cannot be restored "
			"here. Nothing was changed."_q;
	case SyncConfigRestoreStatus::InvalidReason:
		return u"That restore is not allowed. Nothing was changed."_q;
	case SyncConfigRestoreStatus::InvalidSettings:
		return u"settings.toml is not a regular file or is too large, so it "
			"was not replaced."_q;
	case SyncConfigRestoreStatus::HistoryError:
		return u"Could not keep a copy of the current settings in History, "
			"so nothing was changed."_q;
	case SyncConfigRestoreStatus::WriteError:
		return u"Could not write settings.toml. The previous file is in "
			"History."_q;
	}
	return QString();
}

} // namespace Purple

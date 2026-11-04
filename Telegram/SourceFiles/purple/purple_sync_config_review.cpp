/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_config_review.h"

#include "purple/purple_sync_chat_scope.h"

#include "base/platform/base_platform_info.h"
#include "purple/purple_config.h"
#include "purple/purple_settings.h"
#include "purple/purple_sync_account_binding.h"
#include "purple/purple_sync_account_recovery.h"
#include "purple/purple_sync_local_store.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <algorithm>

namespace Purple {
namespace {

[[nodiscard]] bool SettingsTextLoads(
		const QByteArray &text,
		const QString &path) {
	return CanApplyAccountSettingsDocument(u"settings.toml"_q, text);
}

[[nodiscard]] bool SettingsTextNewer(const QByteArray &text) {
	auto input = ConfigImpactInput();
	input.current = { text, true };
	input.proposed.present = false;
	return AnalyzeConfigChange(input).status == ConfigImpactStatus::NewerSchema;
}

[[nodiscard]] bool LastGoodSettingsLoad(const QString &path) {
	const auto info = QFileInfo(path);
	if (info.isSymLink() || !info.isFile() || !AccountFilePathSafe(path)
		|| info.size() > kSyncSettingsMaximumBytes) { return false; }
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) { return false; }
	const auto bytes = file.read(kSyncSettingsMaximumBytes + 1);
	return file.error() == QFileDevice::NoError && file.atEnd()
		&& bytes.size() <= kSyncSettingsMaximumBytes && SettingsTextLoads(bytes, path);
}

SyncConfigReview ClientSupportedReview(SyncConfigReview result) {
	if (result.local.status == SyncSettingsFileStatus::Present && SettingsTextNewer(result.local.text)) {
		result.status = SyncConfigReviewStatus::NeedsReview;
		result.reason = SyncConfigReviewReason::NewerSchema;
		return result;
	}
	for (const auto &head : result.heads) {
		const auto parsed = ParseSettings(QString::fromUtf8(head.text), u"sync record"_q);
		if (!parsed.ok() || !CanApplyAccountSettingsDocument(u"settings.toml"_q, head.text)) {
			result.status = SyncConfigReviewStatus::NeedsReview;
			result.reason = SettingsTextNewer(head.text)
				? SyncConfigReviewReason::NewerSchema
				: SyncConfigReviewReason::Unreadable;
			break;
		}
	}
	return result;
}

} // namespace

QString SyncDeviceName(const SyncDeviceNameParts &parts) {
	const auto name = parts.platform.isEmpty() ? u"Device"_q : parts.platform;
	return parts.shortId.isEmpty() ? name : (name + u' ' + parts.shortId);
}

QString SyncDeviceName(const QString &platform, const QString &install) {
	return SyncDeviceName(SyncDeviceNameOf(platform, install));
}

QString SyncDeviceName(const SyncConfigHeadRecord &record) {
	return SyncDeviceName(SyncDeviceNameOf(record));
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

SyncConfigWriter SyncWriter() {
	return { .platform = SyncWriterPlatform(), .app = SyncWriterApp() };
}

SyncSettingsFile ReadSyncSettingsFile(const AccountFiles &files) {
	const auto &path = files.settings;
	const auto lastGood = false;
	const auto invalid = MakeSyncSettingsFile(
		SyncSettingsFileStatus::Invalid,
		QByteArray(),
		lastGood);
	const auto info = QFileInfo(path);
	if (!info.exists() && !info.isSymLink()) {
		return MakeSyncSettingsFile(
			SyncSettingsFileStatus::Absent,
			QByteArray(),
			lastGood || LastGoodSettingsLoad(files.good));
	} else if (info.isSymLink()
		|| !info.isFile()
		|| info.size() > kSyncSettingsMaximumBytes) {
		return invalid;
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return invalid;
	}
	const auto text = file.read(kSyncSettingsMaximumBytes + 1);
	if (file.error() != QFileDevice::NoError
		|| text.size() > kSyncSettingsMaximumBytes
		|| !file.atEnd()) {
		return invalid;
	}
	return MakeSyncSettingsFile(
		SyncSettingsFileStatus::Present,
		text,
		lastGood
			|| (!SettingsTextNewer(text) && !SettingsTextLoads(text, path) && LastGoodSettingsLoad(files.good)));
}

bool SyncCarryLeftoversPresent(const QString &syncDirectory) {
	for (const auto &name : { u"carry.json"_q, u"carry-cache.json"_q, u"carry-objects"_q, u"carry-history"_q }) {
		if (SyncPathIdentity(syncDirectory + u'/' + name) != "missing") { return true; }
	}
	return false;
}

std::optional<SyncFirstUseMark> ReadSyncFirstUseMark(const AccountFiles &files) {
	const auto path = files.syncDirectory + u"/first-use.json"_q;
	const auto identity = SyncPathIdentity(path);
	if (identity == "missing") {
		return SyncFirstUseMark();
	}
	auto bytes = QByteArray();
	if (identity.isEmpty() || ReadSyncPrivateFile(path, 4096, bytes) != SyncStoreStatus::Ready) {
		return std::nullopt;
	}
	const auto object = QJsonDocument::fromJson(bytes).object();
	if (object.value(u"version"_q).toInt() != 1) {
		return std::nullopt;
	}
	const auto choice = object.value(u"choice"_q).toString();
	auto result = SyncFirstUseMark();
	result.choice = (choice == u"synced"_q) ? SyncFirstUseChoice::Synced
		: (choice == u"this_device"_q) ? SyncFirstUseChoice::ThisDevice
		: SyncFirstUseChoice::None;
	result.joinSpace = object.value(u"join_space"_q).toString();
	result.checkedAt = int64_t(object.value(u"checked_at"_q).toDouble());
	result.fingerprint = object.value(u"fingerprint"_q).toString();
	for (const auto &head : object.value(u"heads"_q).toArray()) {
		result.heads.push_back(head.toString());
	}
	return result;
}

bool WriteSyncFirstUseMark(const AccountFiles &files, const SyncFirstUseMark &mark) {
	auto object = QJsonObject();
	object.insert(u"version"_q, 1);
	object.insert(u"choice"_q, (mark.choice == SyncFirstUseChoice::Synced) ? u"synced"_q
		: (mark.choice == SyncFirstUseChoice::ThisDevice) ? u"this_device"_q
		: u"none"_q);
	object.insert(u"join_space"_q, mark.joinSpace);
	object.insert(u"checked_at"_q, double(mark.checkedAt));
	object.insert(u"fingerprint"_q, mark.fingerprint);
	object.insert(u"heads"_q, QJsonArray::fromStringList(QStringList(mark.heads.begin(), mark.heads.end())));
	return EnsureSyncPrivateDirectory(files.syncDirectory) == SyncStoreStatus::Ready
		&& WriteSyncPrivateFile(files.syncDirectory + u"/first-use.json"_q,
			QJsonDocument(object).toJson(QJsonDocument::Compact)) == SyncStoreStatus::Ready
		&& ReadSyncFirstUseMark(files) == mark;
}

bool ClearSyncFirstUseMark(const AccountFiles &files) {
	const auto path = files.syncDirectory + u"/first-use.json"_q;
	return SyncPathIdentity(path) == "missing" || QFile::remove(path);
}

SyncFirstUseReview ReviewSyncFirstUse(
		const AccountFiles &files,
		const SyncAccountInventoryResult &inventory,
		SyncFirstUseChoice choice) {
	auto result = SyncFirstUseReview();
	result.carry = SyncCarryLeftoversPresent(files.syncDirectory);
	const auto snapshot = ReadSyncRecoveryStore(files, std::nullopt);
	if (snapshot.status != SyncStoreStatus::Ready) { return result; }
	const auto mark = ReadSyncFirstUseMark(files);
	const auto &selected = inventory.directory.selectedSpace;
	if (!snapshot.store.statePresent) {
		if (!snapshot.store.pendingFiles.empty()) { return result; }
		result.need = SyncFirstUseNeed::NoState;
	} else {
		const auto parsed = ParseSyncLocalState(snapshot.store.state);
		if (!parsed || !selected) {
			return result;
		} else if (*selected != parsed.state.space) {
			result.need = SyncFirstUseNeed::OtherSpace;
		} else if (mark && mark->joinSpace == *selected && parsed.state.config.seq == 0
			&& !parsed.state.config.pendingSeq && parsed.state.configData.base.isEmpty()
			&& parsed.state.configData.seenSeq.empty()) {
			result.need = SyncFirstUseNeed::Unjoined;
		} else {
			result.settled = true;
			return result;
		}
	}
	auto local = ReadSyncSettingsFile(files);
	if (local.status == SyncSettingsFileStatus::Present && !local.usingLastGood
		&& !SettingsTextNewer(local.text) && !SettingsTextLoads(local.text, files.settings)) {
		local.status = SyncSettingsFileStatus::Invalid;
	}
	auto input = SyncFirstUseInputOf(inventory, local);
	if (!input) { return result; }
	auto heads = input->expectedParents;
	if (heads.empty() && input->headKey) {
		heads.push_back(*input->headKey);
	}
	std::sort(heads.begin(), heads.end());
	if (choice == SyncFirstUseChoice::None && mark && mark->heads == heads) {
		choice = mark->choice;
	}
	result.heads = heads;
	input->choice = choice;
	result.input = *input;
	result.action = PlanSyncFirstUse(*input);
	result.headKey = input->headKey;
	result.sameBytes = input->head && input->head->fingerprint == local.fingerprint;
	result.request.expectedFingerprint = local.fingerprint;
	result.request.expectedParents = input->expectedParents;
	return result;
}

SyncConfigReview ReviewSyncConfig(
		Main::Account &account,
		Main::Session &session,
		const SyncAccountInventoryResult &inventory, PeerToken chat) {
	auto result = SyncConfigReview();
	result.accountUserId = inventory.accountUserId;
	if (!SyncAccountAvailable(account, session, inventory.accountUserId)) {
		result.status = SyncConfigReviewStatus::AccountUnavailable;
		return result;
	}
	const auto recovery = InspectSyncAccountRecovery(session, &inventory);
	if (!recovery || (recovery->plan.status != SyncAccountRecoveryStatus::Setup
		&& recovery->plan.status != SyncAccountRecoveryStatus::Bound)) {
		result.status = !recovery ? SyncConfigReviewStatus::NeedsReview
			: recovery->plan.status == SyncAccountRecoveryStatus::StartAgain ? SyncConfigReviewStatus::StoreError
			: recovery->plan.status == SyncAccountRecoveryStatus::RootRelink
				|| recovery->plan.status == SyncAccountRecoveryStatus::AutoRelink ? SyncConfigReviewStatus::AccountUnbound
			: SyncConfigReviewStatus::NeedsReview;
		if (result.status == SyncConfigReviewStatus::NeedsReview) {
			result.reason = (recovery && recovery->plan.status == SyncAccountRecoveryStatus::NeedsReview)
				? SyncConfigReviewReason::Unreadable
				: SyncConfigReviewReason::Ambiguous;
		}
		return result;
	}
	const auto target = AccountSyncInspectionTarget(&session);
	if (!target) { result.status = SyncConfigReviewStatus::AccountUnavailable; return result; }
	if (QFileInfo::exists(target->files.syncDirectory + u"/recovery.json"_q)
		|| QFileInfo(target->files.syncDirectory + u"/recovery.json"_q).isSymLink()) {
		result.status = SyncConfigReviewStatus::StoreError;
		return result;
	}
	const auto local = ReadSyncSettingsFile(target->files);
	const auto snapshot = ReadSyncRecoveryStore(target->files, std::nullopt);
	if (snapshot.status != SyncStoreStatus::Ready) { result.status = SyncConfigReviewStatus::StoreError; return result; }
	if (!snapshot.store.statePresent) {
		if (snapshot.store.pendingPresent) { result.status = SyncConfigReviewStatus::StoreError; return result; }
		return ClientSupportedReview(ReviewSyncConfigInventory(inventory, nullptr, {}, local));
	}
	const auto parsed = ParseSyncLocalState(snapshot.store.state);
	if (!parsed) { result.status = SyncConfigReviewStatus::StoreError; return result; }
	const auto state = &parsed.state;
	if (!DesktopSyncChatLedgerMatches(*state, chat)) {
		const auto &selected = inventory.directory.selectedSpace;
		result.status = SyncConfigReviewStatus::NeedsReview;
		result.reason = (selected && *selected != state->space)
			? SyncConfigReviewReason::SpaceMismatch
			: SyncConfigReviewReason::OwnRecords;
		return result;
	}
	if (CheckAccountSyncBinding(*state, account) != SyncAccountBindingVerdict::Bound) {
		result.status = SyncConfigReviewStatus::AccountUnbound;
		return result;
	}
	auto staged = QByteArray();
	if (state->config.pendingSeq) {
		const auto name = u"config-%1.json"_q.arg(state->config.pendingSeq);
		const auto found = snapshot.store.pendingFiles.find(name);
		if (found == snapshot.store.pendingFiles.end()) { result.status = SyncConfigReviewStatus::StoreError; return result; }
		staged = found->second;
	}
	return ClientSupportedReview(ReviewSyncConfigInventory(inventory, state, staged, local));
}

} // namespace Purple

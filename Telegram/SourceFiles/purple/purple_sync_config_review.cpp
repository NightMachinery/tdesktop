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
		return result;
	}
	for (const auto &head : result.heads) {
		const auto parsed = ParseSettings(QString::fromUtf8(head.text), u"sync record"_q);
		if (!parsed.ok() || !CanApplyAccountSettingsDocument(u"settings.toml"_q, head.text)) {
			result.status = SyncConfigReviewStatus::NeedsReview;
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
	const auto recovery = InspectSyncAccountRecovery(session, &inventory);
	if (!recovery || (recovery->plan.status != SyncAccountRecoveryStatus::Setup
		&& recovery->plan.status != SyncAccountRecoveryStatus::Bound)) {
		result.status = !recovery ? SyncConfigReviewStatus::NeedsReview
			: recovery->plan.status == SyncAccountRecoveryStatus::StartAgain ? SyncConfigReviewStatus::StoreError
			: recovery->plan.status == SyncAccountRecoveryStatus::RootRelink
				|| recovery->plan.status == SyncAccountRecoveryStatus::AutoRelink ? SyncConfigReviewStatus::AccountUnbound
			: SyncConfigReviewStatus::NeedsReview;
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
	if (!DesktopSyncChatLedgerMatches(*state, {})) {
		result.status = SyncConfigReviewStatus::NeedsReview;
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

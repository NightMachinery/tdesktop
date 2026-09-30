/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_config_review.h"

#include "base/platform/base_platform_info.h"
#include "purple/purple_config.h"
#include "purple/purple_sync_account_binding.h"
#include "purple/purple_sync_local_store.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>

namespace Purple {

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

SyncSettingsFile ReadSyncSettingsFile(const QString &path) {
	const auto info = QFileInfo(path);
	if (!info.exists() && !info.isSymLink()) {
		return MakeSyncSettingsFile(SyncSettingsFileStatus::Absent);
	} else if (info.isSymLink()
		|| !info.isFile()
		|| info.size() > kSyncSettingsMaximumBytes) {
		return SyncSettingsFile();
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return SyncSettingsFile();
	}
	const auto text = file.read(kSyncSettingsMaximumBytes + 1);
	if (file.error() != QFileDevice::NoError
		|| text.size() > kSyncSettingsMaximumBytes
		|| !file.atEnd()) {
		return SyncSettingsFile();
	}
	return MakeSyncSettingsFile(SyncSettingsFileStatus::Present, text);
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
			return result;
		}
		staged = pending.staged;
	}
	return ReviewSyncConfigInventory(inventory, state, staged, local);
}

} // namespace Purple

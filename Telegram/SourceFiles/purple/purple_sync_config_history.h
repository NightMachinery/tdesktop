/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_account_files.h"
#include <functional>
#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <optional>
#include <vector>

namespace Purple {

enum class SyncConfigHistoryReason {
	BeforeUpdate,
	BeforeChoice,
	BeforeRestore,
	BeforeUndo,
};

struct SyncConfigHistoryEntry {
	QString id;
	int64 createdMs = 0;
	SyncConfigHistoryReason reason = SyncConfigHistoryReason::BeforeUpdate;
	QString label;
	QString versionKey;
	QString fingerprint;
	int64 size = 0;
	bool existed = true;
};

struct SyncHistoryAccess {
	AccountFiles files;
	std::function<bool()> valid;
};

[[nodiscard]] QString SyncConfigHistoryDirectory(const SyncHistoryAccess &access);
[[nodiscard]] std::optional<SyncConfigHistoryEntry> SaveSyncConfigHistory(
	const SyncHistoryAccess &access,
	const std::optional<QByteArray> &text,
	SyncConfigHistoryReason reason,
	const QString &label,
	const QString &versionKey = QString(),
	const QString &keepId = QString());
[[nodiscard]] std::vector<SyncConfigHistoryEntry> ListSyncConfigHistory(const SyncHistoryAccess &access);
[[nodiscard]] std::optional<QByteArray> ReadSyncConfigHistory(
	const SyncHistoryAccess &access,
	const QString &id);

} // namespace Purple

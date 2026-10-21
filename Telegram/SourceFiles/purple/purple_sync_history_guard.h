/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_sync_config_history.h"

#include <QtCore/QPointer>
#include <functional>
#include <utility>

namespace Purple {

[[nodiscard]] inline std::function<bool(const SyncConfigHistoryEntry &)> GuardSyncHistoryRestore(
		QObject *origin, std::function<bool(const SyncConfigHistoryEntry &)> restore) {
	const auto guard = QPointer<QObject>(origin);
	return [guard, restore = std::move(restore)](const SyncConfigHistoryEntry &entry) {
		return guard && restore && restore(entry);
	};
}

}

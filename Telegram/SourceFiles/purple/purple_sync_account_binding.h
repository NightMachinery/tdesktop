/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_sync_local_state.h"

#include <cstdint>

namespace Main {
class Account;
class Session;
}

namespace Purple {

[[nodiscard]] QByteArray AccountSyncBindingToken(Main::Account &account);
[[nodiscard]] SyncAccountBindingVerdict CheckAccountSyncBinding(
	const SyncLocalState &state,
	Main::Account &account);
[[nodiscard]] bool StoreAccountSyncBinding(
	const SyncLocalState &state,
	Main::Account &account);
[[nodiscard]] bool SyncAccountAvailable(
	Main::Account &account,
	Main::Session &session,
	uint64_t accountUserId);

}

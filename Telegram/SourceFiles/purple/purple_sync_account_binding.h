/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_sync_local_state.h"

namespace Main {
class Account;
}

namespace Purple {

[[nodiscard]] SyncAccountBindingVerdict CheckAccountSyncBinding(
	const SyncLocalState &state,
	Main::Account &account);
[[nodiscard]] bool StoreAccountSyncBinding(
	const SyncLocalState &state,
	Main::Account &account);

}

/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_account_binding.h"

#include "data/data_user.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "storage/storage_account.h"

namespace Purple {

constexpr auto kBindingTokenPref = "purple.sync.binding_token";

QByteArray AccountSyncBindingToken(Main::Account &account) {
	return account.local().readPref<QByteArray>(kBindingTokenPref);
}

SyncAccountBindingVerdict CheckAccountSyncBinding(
		const SyncLocalState &state,
		Main::Account &account) {
	return CheckSyncAccountBinding(state, AccountSyncBindingToken(account));
}

bool StoreAccountSyncBinding(
		const SyncLocalState &state,
		Main::Account &account) {
	const auto token = state.bindingToken.toLatin1();
	if (CheckSyncAccountBinding(state, token)
		!= SyncAccountBindingVerdict::Bound) {
		return false;
	}
	const auto existing = AccountSyncBindingToken(account);
	if (!existing.isEmpty()) {
		return existing == token;
	}
	account.local().writePref<QByteArray>(kBindingTokenPref, token);
	return CheckAccountSyncBinding(state, account)
		== SyncAccountBindingVerdict::Bound;
}

bool SyncAccountAvailable(
		Main::Account &account,
		Main::Session &session,
		uint64_t accountUserId) {
	return !account.loggingOut()
		&& account.maybeSession() == &session
		&& accountUserId
		&& accountUserId == peerToUser(session.user()->id).bare;
}

}

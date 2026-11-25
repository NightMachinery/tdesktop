/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_account_binding.h"

#include "data/data_user.h"
#include "purple/purple_config.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "storage/storage_account.h"

#include <rpl/event_stream.h>

namespace Purple {
namespace {

constexpr auto kBindingTokenPref = "purple.sync.binding_token";
rpl::event_stream<> Changes;

} // namespace

QByteArray AccountSyncBindingToken(Main::Account &account) {
	return account.local().readPref<QByteArray>(kBindingTokenPref);
}

rpl::producer<> AccountSyncBindingChanges() {
	return Changes.events();
}

SyncAccountBindingVerdict CheckAccountSyncBinding(
		const SyncLocalState &state,
		Main::Account &account) {
	return CheckSyncAccountBinding(state, AccountSyncBindingToken(account));
}

bool RestoreAccountSyncBinding(
		const SyncLocalState &state, Main::Account &account, const QByteArray &expected) {
	const auto token = state.bindingToken.toLatin1();
	if (account.loggingOut() || !account.maybeSession()
		|| AccountSyncBindingToken(account) != expected
		|| CheckSyncAccountBinding(state, token) != SyncAccountBindingVerdict::Bound) { return false; }
	account.local().writePref<QByteArray>(kBindingTokenPref, token);
	const auto stored = AccountSyncBindingToken(account) == token;
	if (stored && expected != token) {
		Changes.fire({});
	}
	return stored;
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
	const auto stored = CheckAccountSyncBinding(state, account)
		== SyncAccountBindingVerdict::Bound;
	if (stored) {
		Changes.fire({});
	}
	return stored;
}

bool SyncAccountAvailable(
		Main::Account &account,
		Main::Session &session,
		uint64_t accountUserId) {
	return AccountSyncOperationsAllowed(&session)
		&& !account.loggingOut()
		&& account.maybeSession() == &session
		&& accountUserId
		&& accountUserId == peerToUser(session.user()->id).bare;
}

}

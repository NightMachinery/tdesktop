/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_session_settings.h"

#include "base/flat_map.h"
#include "base/weak_ptr.h"
#include "crl/crl_on_main.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "storage/storage_account.h"

namespace Purple {
namespace {

constexpr auto kOfferMessagePref = "purple.sync.settings_offer_message_id";

struct SessionState {
	qsizetype rewrittenBytes = 0;
	bool offerStarted = false;
	bool rewritePending = false;
};

base::flat_map<Main::Session*, std::shared_ptr<SessionState>> States;

std::shared_ptr<SessionState> StateFor(not_null<Main::Session*> session) {
	const auto key = session.get();
	const auto existing = States.find(key);
	if (existing != States.end()) {
		return existing->second;
	}
	const auto state = std::make_shared<SessionState>();
	States.emplace(key, state);
	session->lifetime().add([=] {
		States.erase(key);
	});
	const auto weak = base::make_weak(session.get());
	crl::on_main(session, [=] {
		if (session->account().loggingOut()
			|| session->account().maybeSession() != session.get()) {
			return;
		}
		state->rewritePending = true;
		session->saveSettings();
		state->rewritePending = false;
		const auto live = weak.get();
		if (live
			&& live->account().maybeSession() == live
			&& !live->account().loggingOut()
			&& state->rewrittenBytes > 0) {
			LOG(("Purple: rewrote SessionSettings, %1 bytes"
				).arg(state->rewrittenBytes));
		}
		state->rewrittenBytes = 0;
	});
	return state;
}

}

void StartSessionSettings(not_null<Main::Session*> session) {
	StateFor(session);
}

MsgId SettingsOfferMessageId(not_null<Main::Session*> session) {
	const auto bytes = session->local().readPref<QByteArray>(kOfferMessagePref);
	auto ok = false;
	const auto id = MsgId(bytes.toLongLong(&ok));
	return (ok && IsServerMsgId(id)) ? id : MsgId(0);
}

void SetSettingsOfferMessageId(not_null<Main::Session*> session, MsgId id) {
	session->local().writePref<QByteArray>(
		kOfferMessagePref,
		QByteArray::number(qint64(id.bare)));
}

bool TakeSettingsOfferStart(not_null<Main::Session*> session) {
	const auto state = StateFor(session);
	if (state->offerStarted) {
		return false;
	}
	state->offerStarted = true;
	return true;
}

void ReleaseSettingsOfferStart(not_null<Main::Session*> session) {
	StateFor(session)->offerStarted = false;
}

void SessionSettingsWritten(
		not_null<Main::Account*> account,
		const Main::SessionSettings *settings,
		qsizetype bytes) {
	const auto session = account->maybeSession();
	if (!session || account->loggingOut() || bytes <= 0) {
		return;
	}
	const auto state = States.find(session);
	if (state == States.end()
		|| !state->second->rewritePending
		|| settings != &session->settings()) {
		return;
	}
	state->second->rewritePending = false;
	state->second->rewrittenBytes = bytes;
}

}

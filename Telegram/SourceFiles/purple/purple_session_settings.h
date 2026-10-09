/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_msg_id.h"

namespace Main {
class Account;
class Session;
class SessionSettings;
}

namespace Purple {

void StartSessionSettings(not_null<Main::Session*> session);
[[nodiscard]] MsgId SettingsOfferMessageId(not_null<Main::Session*> session);
void SetSettingsOfferMessageId(not_null<Main::Session*> session, MsgId id);
[[nodiscard]] bool TakeSettingsOfferStart(not_null<Main::Session*> session);
void ReleaseSettingsOfferStart(not_null<Main::Session*> session);
void SessionSettingsWritten(
	not_null<Main::Account*> account,
	const Main::SessionSettings *settings,
	qsizetype bytes);

}

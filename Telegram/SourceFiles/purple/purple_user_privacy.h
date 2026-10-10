/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "api/api_user_privacy.h"

namespace Purple {

[[nodiscard]] bool Representable(
	const MTPVector<MTPPrivacyRule> &rules,
	const Api::UserPrivacy::Rule &parsed);

} // namespace Purple

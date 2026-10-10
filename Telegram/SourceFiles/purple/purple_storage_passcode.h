/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QByteArray>

namespace Purple {

[[nodiscard]] bool TryPersianKeyboardPasscode(
	const QByteArray &passcode,
	Fn<bool(const QByteArray &mapped)> attempt);

} // namespace Purple

/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_storage_passcode.h"

#include "purple/purple_passcode.h"

#include <QtCore/QString>

namespace Purple {

bool TryPersianKeyboardPasscode(
		const QByteArray &passcode,
		Fn<bool(const QByteArray &mapped)> attempt) {
	const auto mapped = PersianKeyboardToEnglish(
		QString::fromUtf8(passcode)).toUtf8();
	return (mapped != passcode) && attempt(mapped);
}

} // namespace Purple

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
namespace {

[[nodiscard]] QByteArray PersianKeyboardCounterpart(
		const QByteArray &passcode) {
	return PersianKeyboardToEnglish(QString::fromUtf8(passcode)).toUtf8();
}

} // namespace

bool TryPersianKeyboardPasscode(
		const QByteArray &passcode,
		Fn<bool(const QByteArray &mapped)> attempt) {
	const auto mapped = PersianKeyboardCounterpart(passcode);
	return (mapped != passcode) && attempt(mapped);
}

void PreferPersianKeyboardPasscode(
		QByteArray &passcode,
		Fn<bool(const QByteArray &candidate)> opens) {
	if (passcode.isEmpty()) {
		return;
	}
	auto mapped = PersianKeyboardCounterpart(passcode);
	if ((mapped != passcode) && !opens(passcode) && opens(mapped)) {
		passcode.fill('\0');
		passcode = std::move(mapped);
	} else {
		mapped.fill('\0');
	}
}

} // namespace Purple

/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QByteArray>

namespace Storage::details {
struct PasscodeKdf;
} // namespace Storage::details

namespace Purple {

[[nodiscard]] bool TryPersianKeyboardPasscode(
	const QByteArray &passcode,
	Fn<bool(const QByteArray &mapped)> attempt);

// Replaces passcode with its Persian-keyboard counterpart when only that
// counterpart opens, so the key derived from it, and any wrap migrated from
// it, use the bytes that actually unlocked. A passcode without a
// counterpart, or one that opens as typed, is left as it is; opens is not
// called for a passcode without a counterpart.
void PreferPersianKeyboardPasscode(
	QByteArray &passcode,
	Fn<bool(const QByteArray &candidate)> opens);

// PreferPersianKeyboardPasscode() for a stored passcode wrap: a candidate
// opens when the key derived from it decrypts keyEncrypted. A wrap that is
// still being created (empty keyEncrypted) never maps the passcode.
void PreferPasscodeForWrap(
	QByteArray &passcode,
	const Storage::details::PasscodeKdf &kdf,
	const QByteArray &salt,
	const QByteArray &keyEncrypted);

} // namespace Purple

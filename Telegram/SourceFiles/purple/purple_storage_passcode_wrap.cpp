/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_storage_passcode.h"

#include "storage/details/storage_file_utilities.h"

namespace Purple {

void PreferPasscodeForWrap(
		QByteArray &passcode,
		const Storage::details::PasscodeKdf &kdf,
		const QByteArray &salt,
		const QByteArray &keyEncrypted) {
	using namespace Storage::details;

	if (keyEncrypted.isEmpty()) {
		return;
	}
	PreferPersianKeyboardPasscode(passcode, [&](const QByteArray &candidate) {
		const auto key = (kdf.kind == 0)
			? CreateLocalKey(candidate, salt)
			: CreatePasscodeKey(candidate, salt, kdf);
		auto data = EncryptedDescriptor();
		return key && DecryptLocal(data, keyEncrypted, key);
	});
}

} // namespace Purple

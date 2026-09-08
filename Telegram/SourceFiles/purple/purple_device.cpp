/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_device.h"

#include "base/openssl_help.h"
#include "purple/purple_config.h"

#include <QtCore/QFile>

#ifdef Q_OS_MAC
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#elif defined Q_OS_WIN // Q_OS_MAC
#include <windows.h>
#endif // Q_OS_WIN

namespace Purple {
namespace {

// The identifier the operating system already keeps for this machine. It is
// used rather than something the app generates because it survives everything
// the app can lose: a wiped tdata, a reinstall, a settings.toml deleted by
// hand. A device that changed its id every time it was reinstalled would need
// its ruleset rewritten with it, which is exactly the hand-editing rulesets
// exist to remove.
//
// Empty when the platform will not say. That is a legitimate answer and the
// engine treats it as one - a device with no id matches only the rulesets that
// asked for no device in particular - so there is nothing to invent here. An
// id the app made up would be a fourth file in the config directory and would
// still be lost by the same reset it was meant to survive.
[[nodiscard]] QString MachineId() {
#ifdef Q_OS_MAC
	// IOPlatformUUID off the root device. Pure C IOKit, so this needs no
	// Objective-C and can live beside the other platforms in one file.
	//
	// MACH_PORT_NULL rather than either named constant: kIOMasterPortDefault
	// is deprecated from macOS 12 and kIOMainPortDefault did not exist before
	// it, so a fork that builds against both deployment targets would have to
	// pick which warning to take. IOKit documents the null port as meaning the
	// default one, which is what both constants are.
	const auto service = IOServiceGetMatchingService(
		MACH_PORT_NULL,
		IOServiceMatching("IOPlatformExpertDevice"));
	if (!service) {
		return QString();
	}
	const auto property = IORegistryEntryCreateCFProperty(
		service,
		CFSTR(kIOPlatformUUIDKey),
		kCFAllocatorDefault,
		0);
	IOObjectRelease(service);
	if (!property) {
		return QString();
	}
	auto result = QString();
	if (CFGetTypeID(property) == CFStringGetTypeID()) {
		// The UUID is 36 characters; the buffer is generous rather than exact
		// so that a future macOS returning something longer is truncated
		// instead of read as an empty id.
		const auto text = static_cast<CFStringRef>(property);
		char buffer[128] = { 0 };
		if (CFStringGetCString(
				text,
				buffer,
				sizeof(buffer),
				kCFStringEncodingUTF8)) {
			result = QString::fromUtf8(buffer);
		}
	}
	CFRelease(property);
	return result;
#elif defined Q_OS_WIN // Q_OS_MAC
	// MachineGuid, written by Windows at install time and left alone since.
	wchar_t buffer[128] = { 0 };
	auto size = DWORD(sizeof(buffer));
	const auto status = RegGetValueW(
		HKEY_LOCAL_MACHINE,
		L"SOFTWARE\\Microsoft\\Cryptography",
		L"MachineGuid",
		RRF_RT_REG_SZ,
		nullptr,
		buffer,
		&size);
	return (status == ERROR_SUCCESS)
		? QString::fromWCharArray(buffer)
		: QString();
#else // Q_OS_MAC || Q_OS_WIN
	// systemd writes /etc/machine-id; the dbus copy is what a system without
	// systemd has, and on most installs the two are the same bytes anyway.
	for (const auto &path : {
		u"/etc/machine-id"_q,
		u"/var/lib/dbus/machine-id"_q,
	}) {
		auto file = QFile(path);
		if (!file.open(QIODevice::ReadOnly)) {
			continue;
		}
		const auto contents = QString::fromUtf8(
			file.read(1024)).trimmed();
		if (!contents.isEmpty()) {
			return contents;
		}
	}
	return QString();
#endif // !Q_OS_MAC && !Q_OS_WIN
}

// "macos", "windows" or "linux" - the names the parser accepts for a platform,
// and the same list the phone reports itself with.
[[nodiscard]] QString ThisPlatform() {
#ifdef Q_OS_MAC
	return u"macos"_q;
#elif defined Q_OS_WIN // Q_OS_MAC
	return u"windows"_q;
#else // Q_OS_MAC || Q_OS_WIN
	return u"linux"_q;
#endif // !Q_OS_MAC && !Q_OS_WIN
}

// The machine id is hashed and cut to eight hex characters before anything
// else sees it. settings.toml is a file people mail to themselves and paste
// into a bug report, and the raw identifier is one the rest of the system uses
// to mean this computer - so it never leaves this function. Eight characters
// is four bytes, which is more than enough to tell apart the handful of
// devices one person carries, and short enough to type into a ruleset.
[[nodiscard]] DeviceIdentity Compute() {
	auto result = DeviceIdentity();
	result.platform = ThisPlatform();
	result.cls = u"desktop"_q;

	const auto machine = MachineId();
	if (machine.isEmpty()) {
		LOG(("Purple: no machine id on this platform, "
			"so rulesets naming a device will not match."));
		return result;
	}
	const auto digest = openssl::Sha256(bytes::make_span(machine.toUtf8()));
	const auto shortened = QByteArray(
		reinterpret_cast<const char*>(digest.data()),
		4).toHex();
	result.id = result.platform + '-' + QString::fromLatin1(shortened);
	return result;
}

} // namespace

const DeviceIdentity &ThisDevice() {
	static const auto result = Compute();
	return result;
}

QString DeviceLabelText(const QString &id) {
	if (id.isEmpty()) {
		return QString();
	}
	const auto named = ActiveSettings().device(id);
	return (named && !named->label.isEmpty()) ? named->label : id;
}

} // namespace Purple

#include "base/basic_types.h"
#include "purple/purple_control_commands.h"
#include <QtCore/QByteArray>
#include <map>
#include <optional>
#include <string_view>
#include <cstdio>

namespace Core {
class Settings final {
public:
#include "control-preference-api.inc"
	int writes = 0;

private:
	template <typename Type> void writePrefImpl(std::string_view key, Type value);
	template <typename Type> std::optional<Type> readPrefImpl(std::string_view key);
	void writePrefGeneric(std::string_view key, const QByteArray &value);
	std::optional<QByteArray> readPrefGeneric(std::string_view key);
	std::map<QByteArray, QByteArray> _prefs;
};

void Settings::writePrefGeneric(std::string_view key, const QByteArray &value) {
	const auto name = QByteArray(key.data(), key.size());
	const auto i = _prefs.find(name);
	if (i != _prefs.end() && i->second == value) return;
	_prefs[name] = value;
	++writes;
}

std::optional<QByteArray> Settings::readPrefGeneric(std::string_view key) {
	const auto i = _prefs.find(QByteArray(key.data(), key.size()));
	return i == _prefs.end() ? std::nullopt : std::make_optional(i->second);
}

#include "control-preference-storage.inc"
}

int main() {
	auto settings = Core::Settings();
	auto failures = 0;
	auto checks = 0;
	const auto check = [&](bool value) { ++checks; if (!value) ++failures; };
	const auto key = std::string_view("automation.socket-path");
	check(settings.readPref<QByteArray>(key, "fallback") == "fallback");
	for (const auto &value : { QByteArray(), QByteArray("/private/owned/s"),
		QByteArray("/private/\xd9\x86/s"), QByteArray("x\0y", 3), QByteArray(1, char(0xff)) }) {
		settings.writePref<QByteArray>(key, value);
		check(settings.readPref<QByteArray>(key) == value);
		const auto saved = settings;
		settings = Core::Settings();
		settings = saved;
		check(settings.readPref<QByteArray>(key) == value);
		const auto writes = settings.writes;
		settings.writePref<QByteArray>(key, value);
		check(settings.writes == writes);
	}
	for (auto i = 0; i < Purple::kControlLockUnitCount; ++i) {
		const auto name = Purple::kControlLockKeys[i];
		const auto fallback = Purple::kControlLockDefaults[i];
		check(settings.readPref<bool>(name, fallback) == fallback);
		for (const auto value : { false, true }) {
			settings.writePref<bool>(name, value);
			const auto saved = settings;
			settings = Core::Settings();
			settings = saved;
			check(settings.readPref<bool>(name, fallback) == value);
		}
	}
	std::printf("control preference storage: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}

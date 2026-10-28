#include "purple/purple_emoji_large.h"
#include "rpl/rpl.h"
#include <QtCore/QtEndian>
#include <cstdio>
#include <limits>
#include <map>
#include <string>

namespace Core {
class Settings {
public:
	std::map<std::string, QByteArray> values;
#include "emoji-large-preference-api.inc"

private:
	template <typename Type>
	void writePrefImpl(std::string_view key, Type value);
	template <typename Type>
	std::optional<Type> readPrefImpl(std::string_view key);
	void writePrefGeneric(std::string_view key, const QByteArray &value) {
		values[std::string(key)] = value;
	}
	std::optional<QByteArray> readPrefGeneric(std::string_view key) {
		const auto i = values.find(std::string(key));
		return i == values.end() ? std::nullopt : std::make_optional(i->second);
	}

};
#include "emoji-large-preference-storage.inc"
struct Application {
	Settings prefs;
	int saves = 0;
	Settings &settings() { return prefs; }
	void saveSettingsDelayed() { ++saves; }
};
Application &App() { static auto instance = Application(); return instance; }
}

namespace Purple {
namespace {
constexpr auto kCapPreference = "purple-emoji-large-cache-bytes";
rpl::event_stream<qint64> CapChanges;
}
#include "emoji-large-preferences.inc"
}

int main() {
	using namespace Purple;
	auto checks = 0; auto failures = 0;
	const auto check = [&](bool value, const char *name) {
		++checks; if (!value) { ++failures; std::printf("FAIL: %s\n", name); }
	};
	auto observed = std::vector<qint64>(); auto lifetime = rpl::lifetime();
	EmojiLargeCacheCapValue() | rpl::on_next([&](qint64 value) { observed.push_back(value); }, lifetime);
	check(EmojiLargeCacheCap() == kEmojiLargeDefaultBytes && observed == std::vector<qint64>{kEmojiLargeDefaultBytes}, "absent preference defaults to two GiB");
	SetEmojiLargeCacheCap(1);
	check(EmojiLargeCacheCap() == kEmojiLargeMinimumBytes && observed.back() == kEmojiLargeMinimumBytes
		&& Core::App().saves == 1, "lowered cap clamps and emits worker obligation");
	SetEmojiLargeCacheCap(1);
	check(observed.size() == 2 && Core::App().saves == 1, "identical normalized preference does not reschedule pruning");
	SetEmojiLargeCacheCap(kEmojiLargeMaximumBytes + 1);
	check(EmojiLargeCacheCap() == kEmojiLargeMaximumBytes && Core::App().saves == 2, "ceiling is eight GiB");
	SetEmojiLargeCacheCap(0);
	check(EmojiLargeCacheCap() == kEmojiLargeDefaultBytes && Core::App().saves == 3, "invalid input selects meaningful default");
	Core::App().prefs.writePref<qint64>(kCapPreference, -999);
	check(EmojiLargeCacheCap() == kEmojiLargeDefaultBytes && Core::App().saves == 3, "corrupt stored preference falls back without automatic writes");
	Core::App().prefs.values[kCapPreference] = QByteArray("bad");
	check(EmojiLargeCacheCap() == kEmojiLargeDefaultBytes && Core::App().saves == 3,
		"truncated storage falls back without automatic writes");
	SetEmojiLargeCacheCap(kEmojiLargeMaximumBytes);
	check(Core::App().prefs.values[kCapPreference].toHex() == "0000000200000000",
		"cap storage uses a fixed eight-byte endian-stable value");
	auto restored = Core::Settings();
	restored.values = Core::App().prefs.values;
	check(restored.readPref<qint64>(kCapPreference, 0) == kEmojiLargeMaximumBytes,
		"restart restores the exact saved cap through the production preference API");
	for (const auto value : { std::numeric_limits<qint64>::min(), qint64(-1), qint64(0),
			std::numeric_limits<qint64>::max() }) {
		restored.writePref<qint64>("integer", value);
		check(restored.readPref<qint64>("integer", 123) == value,
			"integer preference specialization preserves signed endpoints");
	}
	std::printf("emoji literal cap preferences: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}

#include "purple/purple_emoji_export_store.h"
#include "base/basic_types.h"
#include <cstdio>
#include <set>
#include <variant>

namespace Core {
struct RecentEmojiDocument { quint64 id = 0; bool test = false; };
struct RecentEmoji {
	struct Id { std::variant<QString, RecentEmojiDocument> data; } id;
};
}

namespace {
using Purple::EmojiExportEntry;
struct RecentProbe {
	std::set<quint64> _largeIds;
	int rank = 0;
	struct Session { bool isTestMode() const { return false; } } _session;
	Session *session = &_session;
	std::optional<EmojiExportEntry> convert(const Core::RecentEmoji &recent)
#include "emoji-large-recent.inc"
};
}

int main() {
	auto checks = 0; auto failures = 0;
	const auto check = [&](bool value, const char *name) {
		++checks; if (!value) { ++failures; std::printf("FAIL: %s\n", name); }
	};
	auto recent = std::vector<Core::RecentEmoji>();
	for (auto id = quint64(1); id <= 70; ++id) {
		for (auto ordinary = 0; ordinary != 3; ++ordinary) recent.push_back({{QString("ordinary")}});
		recent.push_back({{Core::RecentEmojiDocument{id, false}}});
		recent.push_back({{Core::RecentEmojiDocument{id, false}}});
	}
	auto probe = RecentProbe();
	auto entries = std::vector<EmojiExportEntry>();
	Purple::AppendBoundedEmojiExportEntries(entries, recent, 100000,
		[&](const Core::RecentEmoji &entry) { return probe.convert(entry); });
	check(probe._largeIds.size() == 50, "first fifty unique custom emoji selected");
	check(probe._largeIds.contains(1) && probe._largeIds.contains(50)
		&& !probe._largeIds.contains(51), "ordinary ranks and duplicates do not consume prefetch allowance");
	check(entries.size() == 140 && entries[0].recentRank == 4
		&& entries.back().id == 70, "later custom candidates and original recent rank retained");
	check(!probe.convert({{Core::RecentEmojiDocument{0, false}}})
		&& !probe.convert({{Core::RecentEmojiDocument{999, true}}}),
		"zero and foreign-test-account identities excluded");
	auto bounded = RecentProbe(); auto few = std::vector<EmojiExportEntry>();
	Purple::AppendBoundedEmojiExportEntries(few, recent, 2,
		[&](const Core::RecentEmoji &entry) { return bounded.convert(entry); });
	check(few.size() == 2 && bounded._largeIds == std::set<quint64>{1},
		"prefetch is inside same bounded candidate admission");
	std::printf("emoji literal recent selection: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}

/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_translation_exclusions.h"

#include <cstdio>
#include <cstdlib>

namespace {

int gChecks = 0;
int gFailures = 0;

void Expect(bool ok, const char *what, int line) {
	++gChecks;
	if (!ok) {
		++gFailures;
		std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
	}
}

#define EXPECT(cond, what) Expect(bool(cond), (what), __LINE__)

} // namespace

int main() {
	using namespace Purple;

	// Empty and malformed inputs parse cleanly.
	EXPECT(ParseExcludedTranslationChatIds({}).empty(), "empty byte array");
	EXPECT(ParseExcludedTranslationChatIds(" , , 0 , abc , -5 ").empty(),
		"zero and non-numeric entries ignored");

	// Sorting, trimming and deduplication on parse and serialize.
	const auto parsed = ParseExcludedTranslationChatIds(
		" 400, 100 , 400 , 250, 0, bad ");
	EXPECT((parsed == std::vector<uint64_t>{ 100, 250, 400 }),
		"parsed ids sorted and deduplicated");
	const auto serialized = SerializeExcludedTranslationChatIds(
		{ 400, 0, 100, 400, 250 });
	EXPECT(serialized == QByteArray("100,250,400"),
		"serialized ids deterministic and sorted");
	EXPECT(ParseExcludedTranslationChatIds(serialized) == parsed,
		"serialize/parse round trip");

	// Add, idempotency, and removal.
	auto ids = std::vector<uint64_t>();
	ids = SetExcludedTranslationChatId(ids, 9001, 0, true);
	EXPECT(IsTranslationChatExcluded(ids, 9001), "added chat excluded");
	EXPECT(!IsTranslationChatExcluded(ids, 9002), "other chat not excluded");

	ids = SetExcludedTranslationChatId(ids, 9001, 0, true);
	EXPECT((ids == std::vector<uint64_t>{ 9001 }), "duplicate add idempotent");

	// Migrated group -> supergroup matching and complete clearing on un-exclude.
	const auto legacyChatId = uint64_t(100001);
	const auto supergroupId = uint64_t(200002);
	ids = SetExcludedTranslationChatId(ids, legacyChatId, 0, true);
	EXPECT(IsTranslationChatExcluded(ids, supergroupId, legacyChatId),
		"supergroup inherits legacy chat exclusion");
	ids = SetExcludedTranslationChatId(ids, supergroupId, legacyChatId, false);
	EXPECT(!IsTranslationChatExcluded(ids, supergroupId, legacyChatId),
		"un-excluding supergroup clears legacy chat exclusion too");
	EXPECT(!IsTranslationChatExcluded(ids, legacyChatId),
		"legacy chat id removed");
	EXPECT((ids == std::vector<uint64_t>{ 9001 }),
		"other excluded chats preserved");

	// Re-enabling a chat leaves an empty list serialized cleanly.
	ids = SetExcludedTranslationChatId(ids, 9001, 0, false);
	EXPECT(ids.empty(), "last excluded chat removed");
	EXPECT(SerializeExcludedTranslationChatIds(ids).isEmpty(),
		"empty list serializes to empty bytes");

	// Suppression policy: either Purple exclusion or upstream translation_disabled
	// suppresses automatic chat translation, offers, and the translate bar.
	// Explicit manual Translate / Translate Selected actions remain available.
	EXPECT(ShouldSuppressChatTranslation(true, false),
		"purple exclusion suppresses chat bar and auto-translate");
	EXPECT(ShouldSuppressChatTranslation(true, true),
		"purple exclusion plus upstream disable suppresses chat bar and auto-translate");
	EXPECT(ShouldSuppressChatTranslation(false, true),
		"upstream disable suppresses chat bar and auto-translate");
	EXPECT(!ShouldSuppressChatTranslation(false, false),
		"enabled chat allows chat bar and auto-translate");

	// Large 64-bit PeerId values round-trip without truncation.
	const auto largeChannelPeerId = uint64_t(0x2000000012345678ULL);
	const auto encodedLarge = SerializeExcludedTranslationChatIds(
		{ largeChannelPeerId });
	EXPECT((ParseExcludedTranslationChatIds(encodedLarge)
		== std::vector<uint64_t>{ largeChannelPeerId }),
		"64-bit PeerId values round trip intact");

	std::printf(
		"Translation exclusion checks: %d passed, %d failed\n",
		gChecks - gFailures,
		gFailures);
	return gFailures ? EXIT_FAILURE : EXIT_SUCCESS;
}

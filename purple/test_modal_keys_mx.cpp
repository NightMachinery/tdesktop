#include "purple/purple_keys_mx.h"

#include <iostream>
#include <vector>

namespace {

int Checks = 0;
int Failures = 0;

[[nodiscard]] QString Q(const char *s) {
	return QString::fromUtf8(s);
}

void Expect(bool ok, const char *msg) {
	++Checks;
	if (!ok) {
		++Failures;
		std::cerr << "FAIL: " << msg << "\n";
	}
}

} // namespace

int main() {
	using namespace Purple::ModalMx;

	const std::vector<Candidate> items = {
		{ 0, Q("open-chat-gifs"), Q("GIFs"), Q("chat media"), Q(", g"), true },
		{ 1, Q("open-chat-files"), Q("Files"), Q("chat media"), Q(", f"), true },
		{ 2, Q("open-chat-media"), Q("Media"), Q("chat media"), Q(", v"), true },
		{ 3, Q("open-chat-polls"), Q("Polls"), Q("chat media"), Q(", q"), true },
		{ 4, Q("history-open-calendar"), Q("Calendar"), Q("chat"), Q(", d"), true },
		{ 5, Q("chat-palette"), Q("Open chat"), Q("chats"), Q("SPC ,"), true },
		{ 6, Q("open-chat-menu"), Q("Show chat menu"), Q("chat"), Q(", m"), true },
		{ 7, Q("open-pinned-messages"), Q("Open Pinned Messages"), Q("chat"), Q(", p"), true },
		{ 8, Q("open-archived-chats"), Q("Archived chats"), Q("open"), Q("SPC o a"), true },
		{ 9, Q("preview-chat"), Q("Show chat preview"), Q("chats"), Q(", p"), false },
	};

	// 1. Title match beats shorter group/id-only matches ("Open chat" beats "GIFs" for "chat").
	{
		History h;
		for (int i = 0; i < 5; ++i) {
			h.record(Q("open-chat-gifs"), Q("gifs"));
		}
		const auto r = RankAndFilter(items, h, Q("chat"));
		Expect(r.size() == 10, "chat matches all 10 items (9 avail + 1 unavail)");
		Expect(r[0] == 5, "Open chat (shortest title match) ranks #1 for 'chat'");
		Expect(r[1] == 6 || r[1] == 8, "Title matches occupy top slots before metadata-only matches");
		Expect(r[2] == 6 || r[2] == 8, "All available title matches precede GIFs/Files/Media");
		Expect(r[3] == 0, "Usage breaks ties inside metadata-only tier: GIFs ranks first among metadata matches");
		Expect(r.back() == 9, "Unavailable item ranks last");
	}

	// 2. Usage (amx recency and count) breaks ties inside the title-match tier.
	{
		History h;
		h.record(Q("open-chat-menu"), Q("chat"));
		const auto r = RankAndFilter(items, h, Q("chat"));
		Expect(r[0] == 6, "Recent amx title match 'Show chat menu' beats unused 'Open chat'");
		Expect(r[1] == 5, "Unused shorter title match 'Open chat' follows recent title match");
	}

	// 3. Full title match (tier 3) beats partial title + metadata match (tier 2).
	{
		History h;
		const auto r = RankAndFilter(items, h, Q("open chat"));
		Expect(r.size() >= 4, "multi-token 'open chat' matches several items");
		Expect(r[0] == 5, "'Open chat' (both tokens in title) ranks #1 over 'Open Pinned Messages' (partial title)");
	}

	// 4. Empty query follows amx recent-7 -> frequency -> shorter -> A-Z.
	{
		History h;
		h.record(Q("open-chat-media"), Q(""));
		h.record(Q("open-chat-media"), Q(""));
		h.record(Q("open-pinned-messages"), Q(""));
		const auto r = RankAndFilter(items, h, Q(""));
		Expect(r[0] == 7, "Most recent command ranks #1 on empty query");
		Expect(r[1] == 2, "Second most recent ranks #2 on empty query");
		Expect(r.back() == 9, "Unavailable item stays last on empty query");
	}

	// 5. Orderless initialisms, multi-token orderless, invalid regex fallback.
	{
		History h;
		const auto rInit = RankAndFilter(items, h, Q("opm"));
		Expect(rInit.size() == 1 && rInit[0] == 7, "Initialism 'opm' matches 'Open Pinned Messages'");
		const auto rRev = RankAndFilter(items, h, Q("messages pinned"));
		Expect(rRev.size() == 1 && rRev[0] == 7, "Orderless reversed tokens match");
		const auto rBad = RankAndFilter(items, h, Q("[unclosed"));
		Expect(rBad.empty(), "Invalid regex does not crash and returns empty");
	}

	// 6. Highlight spans for both title and subtitle.
	{
		const auto tokens = ParseTokens(Q("chat"));
		const auto labelSpans = LabelMatchSpans(Q("Open chat"), tokens);
		Expect(labelSpans.size() == 1 && labelSpans[0].start == 5 && labelSpans[0].length == 4,
			"LabelMatchSpans highlights 'chat' in 'Open chat'");
		const auto subNone = LabelMatchSpans(Q("GIFs"), tokens);
		Expect(subNone.empty(), "LabelMatchSpans empty when title did not match");
		const auto subSpans = SubtitleMatchSpans(Q("chat media  ·  :open-chat-gifs"), tokens);
		Expect(subSpans.size() == 1 && subSpans[0].start == 0 && subSpans[0].length == 4,
			"SubtitleMatchSpans highlights 'chat' in subtitle");
		const auto idSpans = SubtitleMatchSpans(Q("media  ·  :open-chat-gifs"), tokens);
		Expect(idSpans.size() == 1 && idSpans[0].start == 16 && idSpans[0].length == 4,
			"SubtitleMatchSpans highlights 'chat' inside :command-id");
	}

	// 7. History stepping (M-p / M-n) and JSON round-trip.
	{
		History h;
		h.record(Q("open-chat-gifs"), Q("gifs"));
		h.record(Q("chat-palette"), Q("open chat"));
		const auto s1 = StepInputHistory(h.inputs, -1, 1, Q("draft"));
		Expect(s1.index == 0 && s1.text == Q("open chat"), "M-p steps to newest query");
		const auto s2 = StepInputHistory(h.inputs, s1.index, 1, Q("draft"));
		Expect(s2.index == 1 && s2.text == Q("gifs"), "M-p steps to older query");
		const auto s3 = StepInputHistory(h.inputs, s1.index, -1, Q("draft"));
		Expect(s3.index == -1 && s3.text == Q("draft"), "M-n restores draft");

		const auto round = History::Deserialize(h.serialize());
		Expect(round.recent == h.recent && round.counts == h.counts && round.inputs == h.inputs,
			"History JSON serialize/deserialize round-trips losslessly");
	}

	std::cout << Checks << " checks, " << Failures << " failures\n";
	return Failures ? 1 : 0;
}

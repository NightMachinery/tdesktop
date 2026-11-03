#include "purple/purple_sync_diag.h"

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <iostream>
#include <set>

int main() {
	auto checks = 0;
	auto failures = 0;
	const auto expect = [&](bool value, const char *name) {
		++checks;
		if (!value) { ++failures; std::cerr << name << '\n'; }
	};
	using namespace Purple;

	auto shapes = true;
	auto unique = std::set<QString>();
	for (const auto &word : SyncDiagVocabulary()) {
		shapes = shapes && SyncDiagWordShape(word);
		unique.insert(word);
	}
	expect(shapes, "every vocabulary word has the word shape");
	expect(unique.size() == SyncDiagVocabulary().size(), "vocabulary words are unique");
	expect(!SyncDiagWordShape(u"chat123"_q), "three digits are not a word");
	expect(!SyncDiagWordShape(u"Saved"_q), "uppercase is not a word");

	expect(SyncDiagToken(u"sync_chat"_q) == u"sync_chat"_q, "a word passes");
	expect(SyncDiagToken(u"My chat"_q) == u"invalid"_q, "free text is refused");
	expect(SyncDiagToken(u"+0000000"_q) == u"invalid"_q, "a phone is refused");
	expect(SyncDiagCount(u"count"_q, 1234567890).value == u"99999+"_q, "an id passed as a count is clamped");
	expect(SyncDiagCount(u"count"_q, -4).value == u"neg"_q, "a negative count is marked");
	expect(SyncDiagCount(u"title"_q, 2).key == u"invalid"_q, "a count key must be a word");
	expect(SyncDiagSeconds(30).value == u"30"_q, "seconds are written");

	const auto line = SyncDiagLine(u"sync_chat"_q, u"lookup_start"_q, {
		SyncDiagCode(u"trigger"_q, u"startup"_q),
	});
	expect(line == u"sync_chat lookup_start trigger=startup"_q, "a line is area event fields");
	const auto request = SyncDiagLine(u"sync_chat"_q, u"request"_q, {
		SyncDiagCode(u"method"_q, u"channels.createchannel"_q),
		SyncDiagCode(u"outcome"_q, u"flood"_q),
		SyncDiagSeconds(42),
	});
	expect(request == u"sync_chat request method=channels.createchannel outcome=flood seconds=42"_q, "a request line");
	const auto smuggled = SyncDiagLine(u"sync_chat"_q, u"lookup_result"_q, {
		SyncDiagReason(u"Family group"_q),
	});
	expect(smuggled == u"sync_chat lookup_result reason=invalid"_q, "a title cannot be a reason");
	auto many = std::vector<SyncDiagField>();
	auto longLine = SyncDiagLine(u"sync_chat"_q, u"request"_q, many);
	for (auto i = 0; i != 100; ++i) {
		longLine = SyncDiagLine(u"sync_chat"_q, u"request"_q, {
			{ u"method"_q, longLine },
		});
	}
	expect(longLine.size() <= kSyncDiagLineLimit, "a line is bounded");

	{
		auto repeats = SyncDiagRepeats();
		const auto body = u"sync_chat request method=messages.getallchats outcome=network"_q;
		auto written = 0;
		for (auto i = 0; i != 10; ++i) {
			const auto decision = repeats.offer(1000 + i, body, u"sync_chat"_q, u"request"_q);
			written += decision.write ? 1 : 0;
			expect(decision.summaries.empty(), "no summary inside the window");
		}
		expect(written == SyncDiagRepeats::kBurst, "three copies per window");
		const auto later = repeats.offer(1000 + SyncDiagRepeats::kWindow, body, u"sync_chat"_q, u"request"_q);
		expect(later.write, "the next window writes again");
		expect(later.summaries.size() == 1 && later.summaries[0].dropped == 7, "the ended window summarizes its drops");
		expect(later.summaries.size() == 1
			&& SyncDiagSummaryLine(later.summaries[0]) == u"diag repeated area=sync_chat event=request count=7"_q,
			"the summary line");
		const auto again = repeats.offer(1000 + SyncDiagRepeats::kWindow + 1, body, u"sync_chat"_q, u"request"_q);
		expect(again.write && again.summaries.empty(), "a summary is written once");
	}
	{
		auto repeats = SyncDiagRepeats();
		const auto body = u"sync_chat lookup_start trigger=auto"_q;
		for (auto i = 0; i != 5; ++i) {
			(void)repeats.offer(5000, body, u"sync_chat"_q, u"lookup_start"_q);
		}
		const auto back = repeats.offer(10, body, u"sync_chat"_q, u"lookup_start"_q);
		expect(back.write, "a clock going backwards ends the window");
		expect(back.summaries.size() == 1 && back.summaries[0].dropped == 2, "and summarizes it");
	}
	{
		auto repeats = SyncDiagRepeats();
		const auto first = u"sync_chat auto_paused reason=chat_gone"_q;
		for (auto i = 0; i != 4; ++i) {
			(void)repeats.offer(0, first, u"sync_chat"_q, u"auto_paused"_q);
		}
		auto evicted = std::vector<SyncDiagRepeats::Summary>();
		for (auto i = 0; i != SyncDiagRepeats::kKeys; ++i) {
			auto decision = repeats.offer(1, u"sync_chat other n="_q + QString::number(i), u"sync_chat"_q, u"request"_q);
			evicted.insert(end(evicted), begin(decision.summaries), end(decision.summaries));
		}
		expect(evicted.size() == 1 && evicted[0].event == u"auto_paused"_q && evicted[0].dropped == 1,
			"an evicted key summarizes its drops");
		const auto fresh = repeats.offer(2, first, u"sync_chat"_q, u"auto_paused"_q);
		expect(fresh.write, "an evicted body starts a new window");
	}
	{
		auto repeats = SyncDiagRepeats();
		const auto body = u"sync_chat wait_honored method=channels.getfullchannel seconds=9"_q;
		for (auto i = 0; i != 6; ++i) {
			(void)repeats.offer(0, body, u"sync_chat"_q, u"wait_honored"_q);
		}
		const auto flushed = repeats.flush();
		expect(flushed.size() == 1 && flushed[0].dropped == 3, "flush summarizes open windows");
		expect(repeats.flush().empty(), "flush clears the counts");
		expect(!repeats.offer(1, body, u"sync_chat"_q, u"wait_honored"_q).write, "flush keeps the window");
	}
	{
		auto lines = QStringList();
		auto now = int64_t(0);
		auto log = SyncDiagLog([&](const QString &line) { lines.push_back(line); }, [&] { return now; });
		for (auto i = 0; i != 5; ++i) {
			log.write(u"sync_chat"_q, u"lookup_start"_q, { SyncDiagCode(u"trigger"_q, u"post"_q) });
		}
		expect(lines.size() == 3, "the log writes three copies");
		now = SyncDiagRepeats::kWindow;
		log.write(u"sync_chat"_q, u"lookup_start"_q, { SyncDiagCode(u"trigger"_q, u"post"_q) });
		expect(lines.size() == 5 && lines[3] == u"diag repeated area=sync_chat event=lookup_start count=2"_q,
			"the summary comes before the next line");
	}
	{
		auto lines = QStringList();
		SyncDiag(u"sync_chat"_q, u"lookup_start"_q, { SyncDiagCode(u"trigger"_q, u"screen"_q) });
		SetSyncDiagSink([&](const QString &line) { lines.push_back(line); });
		SyncDiag(u"sync_chat"_q, u"lookup_start"_q, { SyncDiagCode(u"trigger"_q, u"manual"_q) });
		SetSyncDiagSink(nullptr);
		expect(lines == QStringList{ u"sync_chat lookup_start trigger=manual"_q }, "the global log uses the installed sink");
	}

	std::cout << "sync-diag checks=" << checks << " failures=" << failures << '\n';
	return failures ? 1 : 0;
}

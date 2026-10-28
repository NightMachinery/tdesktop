#include "purple/purple_emoji_large.h"
#include "purple/purple_emoji_export_queue.h"
#include "base/basic_types.h"
#include <QtCore/QCoreApplication>
#include <cstdio>
#include <limits>

int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv);
	using namespace Purple;
	auto checks = 0;
	auto failures = 0;
	const auto check = [&](bool value, const char *name) {
		++checks;
		if (!value) { ++failures; std::printf("FAIL %s\n", name); }
	};
	for (const auto &name : { u"1"_q, u"42"_q, u"18446744073709551615"_q }) {
		check(EmojiLargeRequestId(name).has_value(), "canonical nonzero decimal admitted");
	}
	for (const auto &name : { QString(), u"0"_q, u"01"_q, u"-1"_q, u"+1"_q,
		u" 1"_q, u"1 "_q, u"1\n"_q, u"1.png"_q, u"../1"_q, u"1/2"_q,
		u"18446744073709551616"_q, u"111111111111111111111"_q, u"\u06f1"_q }) {
		check(!EmojiLargeRequestId(name), "noncanonical or path-like name refused");
	}
	check(*EmojiLargeRequestId(u"18446744073709551615"_q) == std::numeric_limits<quint64>::max(), "full unsigned id range");
	check(EmojiLargeRenderSize(EmojiExportSource::Tgs, {}) == QSize(512, 512), "vector has sharp sticker-sized render");
	for (const auto kind : { EmojiExportSource::Webp, EmojiExportSource::Webm }) {
		check(EmojiLargeRenderSize(kind, QSize(100, 100)) == QSize(100, 100), "native raster remains native");
		check(EmojiLargeRenderSize(kind, QSize(64, 100)) == QSize(64, 100), "native aspect ratio retained");
		check(EmojiLargeRenderSize(kind, QSize(512, 256)) == QSize(512, 256), "maximum native side accepted");
		check(EmojiLargeRenderSize(kind, {}).isEmpty(), "unknown native dimensions refuse large render");
		check(EmojiLargeRenderSize(kind, QSize(513, 100)).isEmpty(), "oversized native render bounded");
	}
	check(EmojiExportSourceName(EmojiExportSource::Tgs) == u"tgs"_q
		&& EmojiExportSourceName(EmojiExportSource::Webm) == u"webm"_q
		&& EmojiExportSourceName(EmojiExportSource::Webp) == u"webp"_q, "catalog source labels");
	check(NormalizeEmojiLargeCap(0) == kEmojiLargeDefaultBytes
		&& NormalizeEmojiLargeCap(-1) == kEmojiLargeDefaultBytes, "absent or invalid cap uses default");
	check(NormalizeEmojiLargeCap(1) == kEmojiLargeMinimumBytes, "cap floor");
	check(NormalizeEmojiLargeCap(std::numeric_limits<qint64>::max()) == kEmojiLargeMaximumBytes, "cap ceiling");
	check(NormalizeEmojiLargeCap(512LL * 1024 * 1024) == 512LL * 1024 * 1024, "configured cap retained");
	auto queue = EmojiLargeRequestQueue(2, 250);
	check(queue.offer(0, 0) == EmojiLargeAdmission::Invalid, "invalid id never queues");
	check(queue.offer(1, 0) == EmojiLargeAdmission::Queued, "first request queued");
	check(queue.offer(1, 0) == EmojiLargeAdmission::Duplicate, "duplicate request coalesced");
	check(queue.offer(2, 0) == EmojiLargeAdmission::Queued, "second request queued");
	check(queue.offer(3, 0) == EmojiLargeAdmission::Full && queue.size() == 2, "queue cap enforced");
	check(queue.take(0) == 1, "FIFO request dispatch");
	check(!queue.take(1000), "one request in flight");
	queue.complete(2, true, 1);
	check(queue.size() == 2 && !queue.take(1000), "foreign completion does not free active request");
	queue.complete(1, false, 1);
	check(queue.offer(1, 2) == EmojiLargeAdmission::Cooldown, "failed prefix enters cooldown");
	check(!queue.take(249), "rate limit enforced before next slot");
	check(queue.take(250) == 2, "later request progresses past failing prefix");
	queue.complete(2, true, 250);
	check(queue.offer(1, 60000) == EmojiLargeAdmission::Cooldown, "failure cooldown not shortened by later completion");
	check(queue.offer(1, 60001) == EmojiLargeAdmission::Queued, "failed request eligible after cooldown");
	check(queue.take(60001) == 1, "retry dispatch after cooldown");
	queue.clear();
	check(queue.size() == 0 && !queue.pending() && !queue.take(60002), "clear removes active and pending requests");
	check(queue.offer(1, 0) == EmojiLargeAdmission::Queued && queue.take(0) == 1, "new generation starts without inherited waits");
	queue.complete(1, true, 0);
	check(queue.offer(1, 1) == EmojiLargeAdmission::Queued, "successful id is not permanently suppressed");
	auto bounded = EmojiLargeRequestQueue(1000);
	for (auto id = quint64(1); id <= 64; ++id) check(bounded.offer(id, 0) == EmojiLargeAdmission::Queued, "all bounded slots usable");
	check(bounded.offer(65, 0) == EmojiLargeAdmission::Full, "caller cannot enlarge hard queue bound");
	auto normal = EmojiExportQueue(1);
	normal.begin({10, 20, 30}, {30});
	check(normal.current() == 2, "extra id initially reserved");
	normal.prioritize(1);
	check(normal.current() == 1, "demand advances pending item");
	normal.next();
	check(normal.current() == 2, "reserved extra remains next");
	normal.prioritize(1);
	check(normal.current() == 1, "completed item can be demanded again");
	normal.next();
	check(normal.current() == 2, "repeated demand does not duplicate normal work");
	normal.prioritize(3);
	check(normal.current() == 3, "new entry admitted before remainder");
	normal.next(); normal.next(); normal.next();
	check(!normal.current(), "normal remainder completes exactly once");
	check(normal.resolve(10, 0) == EmojiResolveAdmission::Ready, "budget used");
	normal.prioritize(0);
	check(normal.current() == 0 && normal.resolve(10, 0) == EmojiResolveAdmission::Continue,
		"demand cannot reset resolution budget");
	normal.transient(110, 100, 0);
	normal.prioritize(3);
	normal.continueSlice();
	check(normal.resolve(40, 1) == EmojiResolveAdmission::Cooldown,
		"demand cannot reset shared retry deadline");
	normal.failed(40, 0);
	normal.prioritize(3);
	check(normal.cooling(40, 1), "demand cannot reset failed-id cooldown");
	for (auto i = 0; i != 1000; ++i) {
		normal.prioritize(3);
		normal.next();
	}
	normal.next();
	check(!normal.current(), "repeated completed demands leave no retained duplicates");

	auto titles = EmojiSetTitleCache(2);
	titles.known(10, {u"Installed"_q, u"installed"_q});
	check(!titles.begin(10, 0), "installed metadata makes no request");
	check(titles.lookup(10)->title == u"Installed"_q, "installed title retained");
	check(titles.begin(20, 0), "unknown set starts one request");
	check(!titles.begin(20, 0) && !titles.begin(30, 0), "same and other set deduplicated while one in flight");
	titles.complete(30, {u"Foreign"_q, u"foreign"_q});
	check(titles.active() == 20 && !titles.lookup(30), "foreign reply cannot clear actual request");
	titles.failed(20, 0);
	check(!titles.lookup(20) && !titles.begin(20, 59999), "failed set stays unknown under cooldown");
	check(titles.begin(20, 60000), "failed set allowed at next valid cooldown boundary");
	titles.complete(20, {u"Resolved"_q, u"resolved"_q});
	check(titles.lookup(20)->title == u"Resolved"_q && !titles.begin(20, 60001), "resolved title cached once for all entries");
	check(!titles.begin(30, 60001), "session title cache bounded");
	titles.clear();
	check(!titles.lookup(10) && !titles.active(), "account generation clear drops title state");
	check(titles.begin(40, 0), "new generation title request");
	titles.known(40, {u"Locally installed"_q, u"local"_q});
	titles.complete(40, {u"Older server response"_q, u"old"_q});
	check(titles.lookup(40)->title == u"Locally installed"_q && !titles.active(), "new local metadata wins over late resolution");
	titles.clear();
	check(titles.canBegin(50, 0) && !titles.active(), "admission probe does not allocate request");
	check(titles.begin(50, 0), "set request before transient");
	titles.defer(99);
	check(titles.active() == 50, "foreign transient cannot retire actual request");
	titles.defer(50);
	check(!titles.active() && titles.canBegin(50, 0) && !titles.retryAt(0),
		"transient leaves set unknown and available behind global deadline");
	check(titles.begin(50, 0), "set resumed after shared deadline");
	titles.failed(50, 0);
	check(titles.retryAt(0) == 60000 && !titles.canBegin(50, 59999),
		"permanent error schedules bounded cooldown and keeps empty title");
	check(!titles.retryAt(60000) && titles.canBegin(50, 60000), "no expired retry tight loop");
	std::printf("emoji large policies: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}

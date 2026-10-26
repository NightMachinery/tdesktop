#include "purple/purple_emoji_export_worker.h"
#include "purple/tests/emoji_export_shutdown_cv.h"

#include <QtCore/QCoreApplication>
#include <chrono>
#include <cstdio>

using namespace Purple;
using namespace Purple::Tests;

int main(int argc, char **argv) {
	auto app = QCoreApplication(argc, argv);
	auto checks = 0;
	auto failures = 0;
	const auto check = [&](bool ok, const char *name) {
		++checks;
		if (!ok) {
			++failures;
			std::printf("FAIL %s\n", name);
		}
	};
	for (auto iteration = 0; iteration != 8; ++iteration) {
		EmojiShutdownGapReached.store(false);
		EmojiShutdownReleaseGap.store(false);
		EmojiShutdownNotified.store(false);
		auto worker = new EmojiExportWorker(QString());
		const auto started = std::chrono::steady_clock::now();
		while (!EmojiShutdownGapReached.load()
			&& std::chrono::steady_clock::now() - started < std::chrono::seconds(5)) {
			std::this_thread::yield();
		}
		check(EmojiShutdownGapReached.load(), "worker reached false predicate before underlying wait");
		auto notifiedBeforeRelease = false;
		auto release = std::thread([&] {
			const auto started = std::chrono::steady_clock::now();
			while (!EmojiShutdownNotified.load()
				&& std::chrono::steady_clock::now() - started < std::chrono::milliseconds(100)) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			notifiedBeforeRelease = EmojiShutdownNotified.load();
			EmojiShutdownReleaseGap.store(true);
		});
		delete worker;
		release.join();
		check(!notifiedBeforeRelease, "shutdown notification cannot precede atomic unlock and wait");
	}
	for (auto iteration = 0; iteration != 100; ++iteration) {
		auto worker = EmojiExportWorker(QString());
	}
	check(true, "immediate destruction completed for 100 workers");
	std::printf("emoji shutdown: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}

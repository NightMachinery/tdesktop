/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_sync_history_page.h"

#include <cassert>

using namespace Purple;

int main() {
	auto pages = SyncHistoryPages();
	assert(pages.Add({ { 90, true }, { 80, false } })
		== SyncHistoryPageStatus::More);
	assert(pages.offset() == 80);
	assert(pages.count() == 2);
	assert(pages.Add({ { 70, false } }) == SyncHistoryPageStatus::More);
	assert(pages.count() == 3);
	assert(pages.Add({ { 60, true }, { 50, false } })
		== SyncHistoryPageStatus::More);
	assert((pages.candidates() == std::vector<int32_t>{ 90, 60 }));
	assert(pages.Add({}) == SyncHistoryPageStatus::Complete);

	auto repeated = SyncHistoryPages();
	assert(repeated.Add({ { 40, true } }) == SyncHistoryPageStatus::More);
	assert(repeated.Add({ { 40, true } }) == SyncHistoryPageStatus::Stalled);
	assert(repeated.count() == 1);
	assert((repeated.candidates() == std::vector<int32_t>{ 40 }));
	assert(repeated.Add({ { 35, false }, { 36, true } })
		== SyncHistoryPageStatus::Stalled);
	assert(repeated.offset() == 40);

	auto noOffset = SyncHistoryPages();
	assert(noOffset.Add({ { 0, false } }) == SyncHistoryPageStatus::Stalled);
	assert(noOffset.Add({ { int64_t(1) << 32, true } })
		== SyncHistoryPageStatus::Stalled);
}

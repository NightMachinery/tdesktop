/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <cstdint>
#include <limits>
#include <vector>

namespace Purple {

struct SyncHistoryPageItem {
	int64_t id = 0;
	bool candidate = false;
};

enum class SyncHistoryPageStatus {
	More,
	Complete,
	Stalled,
};

class SyncHistoryPages final {
public:
	[[nodiscard]] SyncHistoryPageStatus Add(
			const std::vector<SyncHistoryPageItem> &page) {
		if (page.empty()) {
			return SyncHistoryPageStatus::Complete;
		}
		auto oldest = int32_t(0);
		auto previous = int64_t(0);
		for (const auto &item : page) {
			if (item.id <= 0
				|| item.id > std::numeric_limits<int32_t>::max()) {
				return SyncHistoryPageStatus::Stalled;
			}
			if ((_offset && item.id >= _offset)
				|| (previous && item.id >= previous)) {
				return SyncHistoryPageStatus::Stalled;
			}
			previous = item.id;
			oldest = int32_t(item.id);
		}
		for (const auto &item : page) {
			if (item.candidate) {
				_candidates.push_back(int32_t(item.id));
			}
		}
		_count += page.size();
		_offset = oldest;
		return SyncHistoryPageStatus::More;
	}

	[[nodiscard]] int32_t offset() const { return _offset; }
	[[nodiscard]] uint64_t count() const { return _count; }
	[[nodiscard]] const std::vector<int32_t> &candidates() const {
		return _candidates;
	}

private:
	std::vector<int32_t> _candidates;
	uint64_t _count = 0;
	int32_t _offset = 0;

};

} // namespace Purple

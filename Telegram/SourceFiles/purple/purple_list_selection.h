#pragma once

#include <algorithm>
#include <optional>
#include <vector>

namespace Purple {

[[nodiscard]] inline int FirstListMatch(int count, const auto &accept) {
	for (auto index = 0; index < count; ++index) {
		if (accept(index)) {
			return index;
		}
	}
	return -1;
}

[[nodiscard]] inline int FirstListMatch(int count) {
	return FirstListMatch(count, [](int) { return true; });
}

template <typename Id>
class ListSelection final {
public:
	void queryEdited() {
		_queryEdited = true;
	}

	void keepCurrentOnClear() {
		_anchor.reset();
		_keepCurrent = true;
	}

	[[nodiscard]] int refilter(
			const std::vector<int> &visible,
			std::optional<Id> selected,
			int previousRow,
			bool filtering,
			const auto &identity) {
		if (filtering && !_filtering) {
			_anchor = selected;
		} else if (!filtering && _anchor) {
			selected = _anchor;
			_anchor.reset();
		}
		_filtering = filtering;
		const auto first = _queryEdited && !_keepCurrent;
		_queryEdited = _keepCurrent = false;
		if (first) {
			return FirstListMatch(int(visible.size()));
		}
		if (visible.empty()) {
			return -1;
		}
		if (selected) {
			const auto found = std::find_if(
				visible.begin(),
				visible.end(),
				[&](int index) { return identity(index) == *selected; });
			if (found != visible.end()) {
				return int(found - visible.begin());
			}
		}
		return std::clamp(previousRow, 0, int(visible.size()) - 1);
	}

private:
	std::optional<Id> _anchor;
	bool _filtering = false;
	bool _queryEdited = false;
	bool _keepCurrent = false;

};

} // namespace Purple

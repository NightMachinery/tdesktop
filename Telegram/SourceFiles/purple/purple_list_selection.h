#pragma once

#include <algorithm>
#include <optional>
#include <vector>

namespace Purple {

template <typename Id>
class ListSelection final {
public:
	void keepCurrentOnClear() {
		_anchor.reset();
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

};

} // namespace Purple

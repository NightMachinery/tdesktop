#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace Purple {

class HintLabels {
public:
	explicit HintLabels(std::size_t count) {
		auto capacity = kLetters.size();
		while (capacity < count) {
			capacity *= kLetters.size();
			++_length;
		}
		if (_length > 1) {
			_short = std::min(capacity / kLetters.size(),
				(capacity - count) / (kLetters.size() - 1));
		}
	}

	[[nodiscard]] std::string operator[](std::size_t index) const {
		const auto shortLabel = index < _short;
		auto result = std::string(_length - shortLabel, ' ');
		auto ordinal = shortLabel ? index : index + _short * (kLetters.size() - 1);
		for (auto i = result.size(); i > 0; --i) {
			result[i - 1] = kLetters[ordinal % kLetters.size()];
			ordinal /= kLetters.size();
		}
		return result;
	}

private:
	static constexpr auto kLetters = std::string_view("asdfghjkl");
	std::size_t _short = 0;
	std::size_t _length = 1;
};

} // namespace Purple

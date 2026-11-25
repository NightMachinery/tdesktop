#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace Purple {

struct HintMnemonic {
	char known = 0;
	std::string title;
};

[[nodiscard]] inline std::vector<std::string> HintMnemonics(
		const std::vector<HintMnemonic> &items) {
	auto used = std::set<char>{ 'j', 'k' };
	auto result = std::vector<std::string>(items.size());
	for (auto i = std::size_t(0); i != items.size(); ++i) {
		if (items[i].known && used.insert(items[i].known).second) {
			result[i] = items[i].known;
		}
	}
	auto overflow = 0;
	for (auto i = std::size_t(0); i != items.size(); ++i) {
		if (!result[i].empty()) {
			continue;
		}
		for (const auto ch : items[i].title) {
			if (ch >= 'a' && ch <= 'z' && used.insert(ch).second) {
				result[i] = ch;
				break;
			}
		}
		for (auto ch = 'a'; result[i].empty() && ch <= 'z'; ++ch) {
			if (used.insert(ch).second) {
				result[i] = ch;
			}
		}
		if (result[i].empty()) {
			auto index = overflow++;
			while (index >= 26) {
				result[i] += ' ';
				index -= 26;
			}
			result[i] += ' ';
			result[i] += char('a' + index);
		}
	}
	return result;
}

} // namespace Purple

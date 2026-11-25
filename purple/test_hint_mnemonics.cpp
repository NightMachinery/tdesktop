#include "purple/purple_hint_mnemonics.h"

#include <iostream>

int main() {
	auto rows = std::vector<Purple::HintMnemonic>{
		{ 0, "reply later" }, { 'r', "reply" }, { 'y', "copy text" },
		{ 'y', "copy selected" }, { 'R', "react" }, { 0, "jk" },
	};
	for (auto i = 0; i != 100; ++i) {
		rows.push_back({ 0, "" });
	}
	const auto keys = Purple::HintMnemonics(rows);
	if (keys[0] != "e" || keys[1] != "r" || keys[2] != "y"
		|| keys[3] != "c" || keys[4] != "R" || keys[5] == "j" || keys[5] == "k"
		|| keys != Purple::HintMnemonics(rows)) {
		return 1;
	}
	for (auto i = std::size_t(0); i != keys.size(); ++i) {
		for (auto j = std::size_t(0); j != keys.size(); ++j) {
			if (i != j && keys[i].starts_with(keys[j])) {
				return 2;
			}
		}
	}
	std::cout << keys.size() << " deterministic, prefix-free menu keys checked\n";
}

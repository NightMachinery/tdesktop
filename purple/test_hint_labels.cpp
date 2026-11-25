#include "purple/purple_hint_labels.h"

#include <iostream>
#include <vector>

int main() {
	auto checks = 0;
	for (const auto count : { 1, 9, 10, 22, 80, 81, 82, 200, 729, 730 }) {
		const auto labels = Purple::HintLabels(count);
		auto seen = std::vector<std::string>();
		for (auto i = 0; i < count; ++i) {
			const auto label = labels[i];
			if (label.find_first_not_of("asdfghjkl") != std::string::npos
				|| (!seen.empty() && seen.back().size() > label.size())) {
				return 1;
			}
			for (const auto &other : seen) {
				if (label.starts_with(other) || other.starts_with(label)) {
					return 2;
				}
			}
			seen.push_back(label);
			++checks;
		}
	}
	const auto labels = Purple::HintLabels(10);
	if (labels[0] != "a" || labels[7] != "k" || labels[8] != "la"
		|| labels[9] != "ls") {
		return 3;
	}
	std::cout << checks << " labels checked, 0 failures\n";
}

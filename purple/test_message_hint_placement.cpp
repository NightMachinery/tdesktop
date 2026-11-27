#include "purple/purple_message_hint_placement.h"

#include <cstdlib>
#include <iostream>

using Purple::MessageHintBadgeRect;

int main() {
	const auto viewport = QRect(0, 0, 200, 100);
	const auto badge = QSize(24, 16);
	const auto gap = 2;
	const auto both = QRect(40, 24, 120, 40);
	const auto wide = QRect(3, 24, 194, 40);
	const auto clipped = QRect(0, 0, 198, 40);
	const auto right = QRect(10, 24, 134, 40);
	const auto left = QRect(56, 24, 134, 40);
	const auto check = [](bool good, const char *name) {
		if (!good) {
			std::cerr << name << '\n';
			std::exit(EXIT_FAILURE);
		}
	};
	check(MessageHintBadgeRect(both, 24, badge, viewport, gap, false) == QRect(14, 24, 24, 16), "leading side");
	check(MessageHintBadgeRect(both, 24, badge, viewport, gap, true) == QRect(162, 24, 24, 16), "mirrored leading side");
	check(MessageHintBadgeRect(right, 24, badge, viewport, gap, false) == QRect(146, 24, 24, 16), "other side when leading edge is tight");
	check(MessageHintBadgeRect(left, 24, badge, viewport, gap, true) == QRect(30, 24, 24, 16), "other side in mirrored layout");
	check(MessageHintBadgeRect(wide, 24, badge, viewport, gap, false) == QRect(0, 16, 24, 16), "straddles corner when both sides are tight");
	check(viewport.contains(MessageHintBadgeRect(clipped, 0, badge, viewport, gap, true)), "clipped top corner stays visible");
	check(MessageHintBadgeRect(both, 45, badge, viewport, gap, false).y() == 45, "link aligns with its own line");
	check(Purple::LayoutHintBadges({ { both, QSize(201, 20) } }, viewport, gap, false).front().badge.isEmpty(),
		"oversized badge omitted");
	check(Purple::WindowHintAnchor(both, badge, false, false).right() == both.right(), "control cap uses trailing edge");
	check(Purple::WindowHintAnchor(both, badge, true, true).right() == both.right(), "RTL row cap uses leading edge");
	check(Purple::HintLinesMeet(QLine(1, 9, 1, 2), QLine(1, 5, 1, 14)), "collinear leaders overlap");
	check(!Purple::HintLinesMeet(QLine(1, 9, 1, 2), QLine(1, 10, 1, 14)), "adjacent leaders separate");
	const auto narrow = Purple::LayoutHintBadges({ { both, QSize(210, 16), true, false, 180 } }, viewport, gap, false);
	check(narrow.front().badge.width() == 180, "compact side padding before omission");
	const auto cluster = std::vector<Purple::HintGeometry>{
		{ QRect(18, 28, 28, 28), badge },
		{ QRect(56, 30, 90, 18), badge },
		{ QRect(56, 54, 64, 20), badge },
		{ QRect(12, 14, 150, 70), badge, false, true },
	};
	for (const auto rtl : { false, true }) {
		const auto result = Purple::LayoutHintBadges(cluster, viewport, gap, rtl);
		check(result == Purple::LayoutHintBadges(cluster, viewport, gap, rtl), "stable cluster layout");
		for (auto i = size_t(0); i != result.size(); ++i) {
			check(!result[i].badge.isEmpty() && viewport.contains(result[i].badge), "audio cluster coverage");
			if (!result[i].leader.isNull()) {
				check(cluster[i].target.contains(result[i].leader.p2()), "endpoint belongs to target");
			} else {
				check(result[i].badge.intersects(cluster[i].target), "attached cap reaches target");
			}
			for (auto j = size_t(0); j < i; ++j) {
				check(!result[i].badge.intersects(result[j].badge), "distinct caps");
				check(!Purple::HintLineHits(result[i].leader, result[j].badge), "no crossing a cap");
				check(!Purple::HintLinesMeet(result[i].leader, result[j].leader), "no crossing leaders");
			}
		}
	}
	std::cout << "Hint placement checks passed\n";
}

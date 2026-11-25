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
	const auto link = QRect(32, 42, 88, 18);
	check(Purple::MessageHintLeaderEnd(QPoint(15, 64), link) == QPoint(31, 60), "leader stops before near corner");
	check(Purple::MessageHintLeaderEnd(QPoint(140, 50), link) == QPoint(120, 50), "leader stops before right edge");
	std::cout << "9 checks, 0 failures\n";
}

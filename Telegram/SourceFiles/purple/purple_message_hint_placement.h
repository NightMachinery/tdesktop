#pragma once

#include <QtCore/QRect>

#include <algorithm>

namespace Purple {

[[nodiscard]] inline QPoint MessageHintLeaderEnd(QPoint from, QRect link) {
	const auto edge = link.adjusted(-1, -1, 1, 1);
	return QPoint(
		std::clamp(from.x(), edge.left(), edge.right()),
		std::clamp(from.y(), edge.top(), edge.bottom()));
}

[[nodiscard]] inline QRect MessageHintBadgeRect(
		QRect bubble,
		int anchorY,
		QSize badge,
		QRect viewport,
		int gap,
		bool rtl) {
	const auto lastX = std::max(viewport.left(), viewport.right() + 1 - badge.width());
	const auto lastY = std::max(viewport.top(), viewport.bottom() + 1 - badge.height());
	const auto xLeft = bubble.left() - badge.width() - gap;
	const auto xRight = bubble.right() + 1 + gap;
	const auto leftFits = xLeft >= viewport.left();
	const auto rightFits = xRight <= lastX;
	const auto y = std::clamp(anchorY, viewport.top(), lastY);
	if (leftFits || rightFits) {
		return QRect(QPoint((leftFits && (!rtl || !rightFits)) ? xLeft : xRight, y), badge);
	}
	const auto leftRoom = bubble.left() - viewport.left();
	const auto rightRoom = viewport.right() - bubble.right();
	const auto useLeft = (leftRoom == rightRoom) ? !rtl : (leftRoom > rightRoom);
	const auto cornerX = useLeft ? bubble.left() : bubble.right() + 1;
	const auto x = std::clamp(cornerX - badge.width() / 2, viewport.left(), lastX);
	const auto cornerY = std::clamp(anchorY - badge.height() / 2, viewport.top(), lastY);
	return QRect(QPoint(x, cornerY), badge);
}

} // namespace Purple

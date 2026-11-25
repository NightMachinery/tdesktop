#pragma once

#include <QtCore/QRect>

#include <algorithm>
#include <vector>

namespace Purple {

[[nodiscard]] inline QRect WindowHintAnchor(QRect target, QSize badge, bool leading, bool rtl) {
	if (target.width() < badge.width() * 2) {
		return QRect(target.center() - QPoint(badge.width() / 2, badge.height() / 2), badge);
	}
	const auto x = (leading != rtl) ? target.left() : target.right() - badge.width() + 1;
	return QRect(QPoint(x, target.center().y() - badge.height() / 2), badge);
}

[[nodiscard]] inline QRect PlaceHintBadge(
		QRect anchor,
		QRect viewport,
		const std::vector<QRect> &placed) {
	if (anchor.isEmpty() || anchor.width() > viewport.width()
		|| anchor.height() > viewport.height()) {
		return {};
	}
	anchor.moveTopLeft(QPoint(
		std::clamp(anchor.x(), viewport.left(), viewport.right() + 1 - anchor.width()),
		std::clamp(anchor.y(), viewport.top(), viewport.bottom() + 1 - anchor.height())));
	const auto free = [&](QRect rect) {
		return viewport.contains(rect) && std::none_of(begin(placed), end(placed),
			[&](QRect other) { return rect.intersects(other); });
	};
	if (free(anchor)) {
		return anchor;
	}
	const auto across = viewport.width() / anchor.width() + 1;
	const auto down = viewport.height() / anchor.height() + 1;
	for (auto distance = 1; distance <= across + down; ++distance) {
		for (auto x = -std::min(across, distance); x <= std::min(across, distance); ++x) {
			const auto y = distance - std::abs(x);
			const auto offset = QPoint(x * anchor.width(), y * anchor.height());
			if (const auto rect = anchor.translated(offset); free(rect)) {
				return rect;
			}
			if (y) {
				if (const auto rect = anchor.translated(offset.x(), -offset.y()); free(rect)) {
					return rect;
				}
			}
		}
	}
	return {};
}

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

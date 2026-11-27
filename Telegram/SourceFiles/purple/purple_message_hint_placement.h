#pragma once

#include <QtCore/QLineF>
#include <QtCore/QRect>

#include <algorithm>
#include <array>
#include <numeric>
#include <vector>

namespace Purple {

[[nodiscard]] inline QRect WindowHintAnchor(QRect target, QSize badge, bool leading, bool rtl) {
	if (target.width() < badge.width() * 2) {
		return QRect(target.center() - QPoint(badge.width() / 2, badge.height() / 2), badge);
	}
	const auto x = (leading != rtl) ? target.left() : target.right() - badge.width() + 1;
	return QRect(QPoint(x, target.center().y() - badge.height() / 2), badge);
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

struct HintGeometry {
	QRect target;
	QSize size;
	bool row = false;
	bool bubble = false;
	int compactWidth = 0;
};

struct HintPlacement {
	QRect badge;
	QLine leader;
	friend bool operator==(const HintPlacement &, const HintPlacement &) = default;
};

[[nodiscard]] inline bool HintLinesMeet(QLine a, QLine b) {
	if (a.isNull() || b.isNull()) {
		return false;
	}
	if (QLineF(a).intersects(b, nullptr) == QLineF::BoundedIntersection) {
		return true;
	}
	const auto onLine = [&](QPoint point) {
		return qint64(a.dx()) * (point.y() - a.y1())
			== qint64(a.dy()) * (point.x() - a.x1());
	};
	return onLine(b.p1()) && onLine(b.p2())
		&& QRect(a.p1(), a.p2()).normalized().intersects(QRect(b.p1(), b.p2()).normalized());
}

[[nodiscard]] inline bool HintLineHits(QLine line, QRect rect) {
	if (line.isNull() || rect.isEmpty()) {
		return false;
	}
	if (rect.contains(line.p1()) || rect.contains(line.p2())) {
		return true;
	}
	for (const auto edge : {
			QLine(rect.topLeft(), rect.topRight()), QLine(rect.topRight(), rect.bottomRight()),
			QLine(rect.bottomRight(), rect.bottomLeft()), QLine(rect.bottomLeft(), rect.topLeft()) }) {
		if (HintLinesMeet(line, edge)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] inline std::vector<HintPlacement> LayoutHintBadges(
		const std::vector<HintGeometry> &items,
		QRect viewport,
		int gap,
		bool rtl) {
	auto result = std::vector<HintPlacement>(items.size());
	auto order = std::vector<size_t>(items.size());
	std::iota(order.begin(), order.end(), size_t(0));
	const auto area = [&](size_t i) {
		return qint64(items[i].target.width()) * items[i].target.height();
	};
	std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
		return area(a) < area(b);
	});
	const auto nearest = [](QRect rect, QPoint point) {
		return QPoint(std::clamp(point.x(), rect.left(), rect.right()),
			std::clamp(point.y(), rect.top(), rect.bottom()));
	};
	for (const auto index : order) {
		const auto &item = items[index];
		const auto target = item.target.intersected(viewport);
		if (target.isEmpty()) {
			continue;
		}
		const auto clear = [&](QRect box, QLine line) {
			for (auto i = size_t(0); i < items.size(); ++i) {
				const auto &placed = result[i];
				if (box.intersects(placed.badge) || HintLineHits(placed.leader, box)
					|| HintLineHits(line, placed.badge) || HintLinesMeet(line, placed.leader)) {
					return false;
				}
				const auto &other = items[i];
				if (i != index && !other.bubble && !other.row && !other.target.contains(target)
					&& (box.intersects(other.target) || HintLineHits(line, other.target))) {
					return false;
				}
			}
			return true;
		};
		const auto place = [&](QSize size) -> HintPlacement {
			const auto w = size.width();
			const auto h = size.height();
			if (viewport.width() < w || viewport.height() < h) {
				return {};
			}
			const auto preferred = item.bubble
				? MessageHintBadgeRect(target, target.top(), size, viewport, gap, rtl)
				: WindowHintAnchor(target, size, item.row, rtl);
			const auto center = target.center();
			const auto y = item.bubble ? target.top() : center.y() - h / 2;
			const auto left = target.left() - w - gap;
			const auto right = target.right() + gap + 1;
			const auto x = center.x() - w / 2;
			auto points = std::vector<QPoint>();
			if (item.row || item.bubble) {
				points.push_back(preferred.topLeft());
			}
			for (const auto offset : { 0, -h, h }) {
				points.push_back(QPoint(rtl ? right : left, y + offset));
				points.push_back(QPoint(rtl ? left : right, y + offset));
				points.push_back(QPoint(x + offset, target.top() - h - gap));
				points.push_back(QPoint(x + offset, target.bottom() + gap + 1));
			}
			if (!item.row && !item.bubble && target.width() >= w * 2) {
				points.push_back(preferred.topLeft());
			}
			for (auto point : points) {
				point.setX(std::clamp(point.x(), viewport.left(), viewport.right() + 1 - w));
				point.setY(std::clamp(point.y(), viewport.top(), viewport.bottom() + 1 - h));
				const auto box = QRect(point, size);
				if (!item.bubble && !item.row && box.contains(center)) {
					continue;
				}
				auto line = QLine();
				if (!box.intersects(target)) {
					const auto end = nearest(target, box.center());
					line = QLine(nearest(box, end), end);
					if (QLineF(line).length() > 2 * h) {
						continue;
					}
				}
				if (clear(box, line)) {
					return { box, line };
				}
			}
			return {};
		};
		result[index] = place(item.size);
		if (result[index].badge.isEmpty()
			&& item.compactWidth > 0 && item.compactWidth < item.size.width()) {
			result[index] = place(QSize(item.compactWidth, item.size.height()));
		}
	}
	return result;
}

} // namespace Purple

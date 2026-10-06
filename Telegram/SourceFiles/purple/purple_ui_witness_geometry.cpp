#include "purple/purple_ui_witness_geometry.h"

#include <cmath>

namespace Purple::UiWitness {

bool IsAxisTranslation(const QTransform &t) {
	return t.m11() == 1. && t.m22() == 1.
		&& t.m12() == 0. && t.m21() == 0.
		&& t.m13() == 0. && t.m23() == 0. && t.m33() == 1.
		&& std::isfinite(t.dx()) && std::isfinite(t.dy())
		&& std::abs(t.dx()) <= 8192 && std::abs(t.dy()) <= 8192
		&& t.dx() == int(t.dx()) && t.dy() == int(t.dy());
}

std::optional<Cells> RectangularCells(
		const QPainterPath &path,
		const QTransform &transform,
		QRectF completeClip) {
	if (!IsAxisTranslation(transform) || path.isEmpty()
		|| path.elementCount() > 8 * 5 || path.elementCount() % 5
		|| completeClip.isEmpty()) {
		return std::nullopt;
	}
	auto cells = Cells();
	for (auto i = 0; i != path.elementCount(); i += 5) {
		const auto a = path.elementAt(i);
		const auto b = path.elementAt(i + 1);
		const auto c = path.elementAt(i + 2);
		const auto d = path.elementAt(i + 3);
		const auto e = path.elementAt(i + 4);
		if (a.type != QPainterPath::MoveToElement
			|| b.type != QPainterPath::LineToElement
			|| c.type != QPainterPath::LineToElement
			|| d.type != QPainterPath::LineToElement
			|| e.type != QPainterPath::LineToElement
			|| a.x != d.x || a.y != b.y || b.x != c.x
			|| c.y != d.y || a.x != e.x || a.y != e.y
			|| b.x <= a.x || c.y <= a.y) {
			return std::nullopt;
		}
		const auto rect = transform.mapRect(QRectF(a.x, a.y, b.x - a.x, c.y - a.y));
		if (!std::isfinite(rect.x()) || !std::isfinite(rect.y())
			|| !std::isfinite(rect.width()) || !std::isfinite(rect.height())
			|| !completeClip.contains(rect) || rect.width() > 240
			|| rect.height() > 48 || rect.width() < 2 || rect.height() < 2) {
			return std::nullopt;
		}
		cells.rectangles[cells.count++] = rect;
	}
	return cells;
}

std::optional<int> LiteralSlot(QStringView text, bool caption) {
	const auto slot = caption ? 1 : 0;
	const auto literal = caption
		? QStringView(u"PURPLE_WITNESS_CAPTION")
		: QStringView(u"PURPLE_WITNESS_BODY");
	return text == literal ? std::optional<int>(slot) : std::nullopt;
}

}

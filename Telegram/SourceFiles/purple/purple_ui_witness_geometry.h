#pragma once

#include "base/basic_types.h"

#include <QPainterPath>
#include <QRectF>
#include <QTransform>
#include <array>
#include <optional>

namespace Purple::UiWitness {

struct Cells {
	std::array<QRectF, 8> rectangles = {};
	int count = 0;
};

[[nodiscard]] bool IsAxisTranslation(const QTransform &transform);
[[nodiscard]] std::optional<Cells> RectangularCells(
	const QPainterPath &path,
	const QTransform &transform,
	QRectF completeClip);
[[nodiscard]] std::optional<int> LiteralSlot(QStringView text, bool caption);

}

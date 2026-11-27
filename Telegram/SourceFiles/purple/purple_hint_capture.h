#pragma once

#include <QtCore/QRect>

#include <vector>

class QPainter;
class QWidget;

namespace Purple {

struct HintCapture {
	std::vector<QRect> text;
	std::vector<QRect> faces;
};

[[nodiscard]] HintCapture CaptureHintGeometry(QWidget *widget, QRect visible);
void RecordHintFace(QPainter &p, QRect rect);

} // namespace Purple

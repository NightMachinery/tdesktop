#include "purple/purple_hint_capture.h"

#include <QtGui/QFontMetricsF>
#include <QtGui/QPainter>
#include <QtGui/QPaintEngine>
#include <QtWidgets/QWidget>

#include <algorithm>

namespace Purple {
namespace {

class HintEngine final : public QPaintEngine {
public:
	explicit HintEngine(HintCapture &capture)
	: QPaintEngine(AllFeatures)
	, _capture(capture) {
	}

	Type type() const override { return User; }
	bool begin(QPaintDevice *) override { return true; }
	bool end() override { return true; }
	void updateState(const QPaintEngineState &) override {}
	void drawPath(const QPainterPath &) override {}
	void drawPixmap(const QRectF &, const QPixmap &, const QRectF &) override {}
	void drawImage(const QRectF &, const QImage &, const QRectF &,
		Qt::ImageConversionFlags) override {}
	void drawTiledPixmap(const QRectF &, const QPixmap &, const QPointF &) override {}

	void drawTextItem(const QPointF &at, const QTextItem &item) override {
		const auto font = QFontMetricsF(item.font(), painter()->device());
		const auto ascent = std::max(item.ascent(), font.ascent());
		const auto descent = std::max(item.descent(), font.descent());
		const auto area = QRectF(at.x(), at.y() - ascent, item.width(), ascent + descent);
		append(*painter(), area, _capture.text);
	}

	void face(QPainter &p, QRect rect) {
		append(p, rect, _capture.faces);
	}

private:
	void append(QPainter &p, QRectF rect, std::vector<QRect> &out) const {
		const auto transform = p.deviceTransform();
		auto region = QRegion(transform.mapRect(rect).toAlignedRect());
		if (const auto clip = systemClip(); !clip.isEmpty()) {
			region &= clip;
		}
		if (p.hasClipping()) {
			region &= transform.map(p.clipRegion());
		}
		for (const auto &part : region) {
			out.push_back(part);
		}
	}

	HintCapture &_capture;
};

class HintDevice final : public QPaintDevice {
public:
	HintDevice(QWidget *widget, HintCapture &capture)
	: _widget(widget)
	, _engine(capture) {
	}

	QPaintEngine *paintEngine() const override { return &_engine; }

protected:
	int metric(PaintDeviceMetric m) const override {
		switch (m) {
		case PdmWidth: return _widget->width();
		case PdmHeight: return _widget->height();
		case PdmWidthMM: return _widget->widthMM();
		case PdmHeightMM: return _widget->heightMM();
		case PdmDepth: return _widget->depth();
		case PdmDpiX: return _widget->logicalDpiX();
		case PdmDpiY: return _widget->logicalDpiY();
		case PdmPhysicalDpiX: return _widget->physicalDpiX();
		case PdmPhysicalDpiY: return _widget->physicalDpiY();
		case PdmDevicePixelRatioF_EncodedA:
		case PdmDevicePixelRatioF_EncodedB: return encodeMetricF(m, 1.);
		default: return QPaintDevice::metric(m);
		}
	}

private:
	QWidget *const _widget;
	mutable HintEngine _engine;
};

} // namespace

HintCapture CaptureHintGeometry(QWidget *widget, QRect visible) {
	auto result = HintCapture();
	auto device = HintDevice(widget, result);
	widget->render(&device, visible.topLeft(), visible);
	std::stable_sort(result.faces.begin(), result.faces.end(), [](QRect a, QRect b) {
		return qint64(a.width()) * a.height() < qint64(b.width()) * b.height();
	});
	return result;
}

void RecordHintFace(QPainter &p, QRect rect) {
	if (p.paintEngine()->type() == QPaintEngine::User) {
		if (const auto engine = dynamic_cast<HintEngine*>(p.paintEngine())) {
			engine->face(p, rect);
		}
	}
}

} // namespace Purple

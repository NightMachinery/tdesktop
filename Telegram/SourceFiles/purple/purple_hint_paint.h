#pragma once

#include "purple/purple_hint_role.h"

#include <QtGui/QPainter>
#include <QtGui/QPainterPath>

#include <algorithm>
#include <array>
#include <cmath>

namespace Purple {

struct HintColors {
	QColor fill;
	QColor edge;
	QColor ink;
	QColor prefix;
};

inline QColor MixHintColor(QColor a, QColor b, double amount) {
	const auto mix = [&](int x, int y) { return qRound(x + (y - x) * amount); };
	return QColor(mix(a.red(), b.red()), mix(a.green(), b.green()), mix(a.blue(), b.blue()));
}

inline double HintLuminance(QColor color) {
	const auto linear = [](double value) {
		return value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4);
	};
	return .2126 * linear(color.redF()) + .7152 * linear(color.greenF()) + .0722 * linear(color.blueF());
}

inline HintColors ResolveHintColors(QColor bg, QColor ink, QColor accent, int percent) {
	const auto surface = QColor(HintLuminance(bg) < .18 ? Qt::black : Qt::white);
	bg = MixHintColor(surface, bg, bg.alphaF());
	ink = MixHintColor(bg, ink, ink.alphaF());
	accent = MixHintColor(bg, accent, accent.alphaF());
	const auto lightInk = HintLuminance(ink) > HintLuminance(bg);
	const auto pole = QColor(lightInk ? Qt::white : Qt::black);
	const auto other = QColor(lightInk ? Qt::black : Qt::white);
	const auto alpha = qRound(std::clamp(percent, 60, 100) * 255. / 100.);
	const auto base = MixHintColor(bg, accent, lightInk ? .30 : .42);
	auto fill = base;
	const auto contrast = [&](QColor fg) {
		const auto near = HintLuminance(MixHintColor(pole, fill, alpha / 255.));
		const auto value = HintLuminance(fg);
		return lightInk ? (value + .05) / (near + .05) : (near + .05) / (value + .05);
	};
	if (contrast(ink) < 4.7) ink = pole;
	for (auto i = 1; contrast(ink) < 4.7 && i <= 100; ++i) {
		fill = MixHintColor(base, other, i / 100.);
	}
	auto prefix = ink;
	for (auto i = 18; i >= 0; --i) {
		prefix = MixHintColor(ink, fill, i / 100.);
		if (contrast(prefix) >= 4.7) break;
	}
	const auto originalEdge = MixHintColor(accent, ink, .48);
	auto edge = originalEdge;
	for (auto i = 1; contrast(edge) < 3.1 && i <= 100; ++i) {
		edge = MixHintColor(originalEdge, ink, i / 100.);
	}
	fill.setAlpha(alpha);
	return { fill, edge, ink, prefix };
}

struct HintPalette {
	std::array<HintColors, int(HintRole::Count)> colors;
	bool symbols = true;
	int markWidth = 0;
	int stroke = 1;
	int radius = 0;

	[[nodiscard]] int inset(HintRole role) const {
		return symbols && role != HintRole::General ? markWidth : 0;
	}
};

[[nodiscard]] HintPalette CurrentHintPalette();

inline void PaintHintBadge(
		QPainter &p,
		QRect rect,
		const QString &label,
		int matched,
		HintRole role,
		const HintPalette &palette,
		bool rtl) {
	p.save();
	p.setRenderHint(QPainter::Antialiasing);
	const auto &colors = palette.colors[int(role)];
	const auto stroke = palette.stroke;
	const auto borderWidth = (role == HintRole::Close) ? 2 * stroke : stroke;
	const auto half = borderWidth / 2.;
	const auto area = QRectF(rect).adjusted(half, half, -half, -half);
	const auto r = std::min<double>(palette.radius, area.height() / 4.);
	const auto l = area.left();
	const auto t = area.top();
	const auto b = area.bottom();
	const auto e = area.right();
	auto outline = QPainterPath();
	if (role == HintRole::Destructive) {
		outline.addPolygon(QPolygonF{
			{ l + r, t }, { e - r, t }, { e, t + r }, { e, b - r },
			{ e - r, b }, { l + r, b }, { l, b - r }, { l, t + r },
		});
		outline.closeSubpath();
	} else if (role == HintRole::Navigate) {
		outline.addPolygon(QPolygonF{
			{ l, t }, { e - r, t }, { e, (b + t) / 2 }, { e - r, b }, { l, b },
		});
		outline.closeSubpath();
	} else if (role == HintRole::Reply) {
		const auto tip = std::min(2 * r, area.width() / 3.);
		outline.addRoundedRect(area.adjusted(0, 0, 0, -r), r, r);
		auto tail = QPainterPath();
		tail.addPolygon(QPolygonF{
			{ l + tip, b - r - half }, { l + tip, b }, { l + 2 * tip, b - r - half },
		});
		tail.closeSubpath();
		outline = outline.united(tail);
	} else {
		const auto capsule = role == HintRole::Playback || role == HintRole::PlayPause;
		const auto radius = capsule ? area.height() / 2 : role == HintRole::Menu ? 0. : r;
		outline.addRoundedRect(area, radius, radius);
	}
	if (rtl && (role == HintRole::Navigate || role == HintRole::Reply)) {
		outline = QTransform(-1, 0, 0, 1, l + e, 0).map(outline);
	}
	p.setPen(QPen(colors.edge, borderWidth));
	p.setBrush(colors.fill);
	p.drawPath(outline);
	auto text = (role == HintRole::Reply) ? rect.adjusted(0, 0, 0, -qRound(r)) : rect;
	const auto inset = palette.inset(role);
	if (inset) {
		const auto x = rtl ? rect.right() - inset / 2 - stroke : rect.left() + inset / 2 + stroke;
		const auto y = text.center().y();
		const auto d = std::min(inset / 3., rect.height() / 4.);
		p.save();
		p.translate(x, y);
		p.scale(rtl ? -d : d, d);
		p.setPen(QPen(colors.edge, std::max(.32, stroke / d), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		p.setBrush(Qt::NoBrush);
		switch (role) {
		case HintRole::Navigate:
			p.drawPolyline(QPolygonF{{ -.5,-.85 },{ .5,0 },{ -.5,.85 }});
			break;
		case HintRole::Reply:
			p.drawPolyline(QPolygonF{{ 1,.6 },{ 1,-.15 },{ -.8,-.15 },{ -.12,-.8 }});
			p.drawLine(QPointF(-.8,-.15), QPointF(-.12,.5));
			break;
		case HintRole::Playback:
		case HintRole::PlayPause:
			if (role == HintRole::Playback) p.setBrush(colors.edge);
			p.drawEllipse(QPointF(), .72, .72);
			break;
		case HintRole::Menu:
			for (const auto dy : { -.7, 0., .7 }) p.drawLine(QPointF(-.75,dy), QPointF(.75,dy));
			break;
		case HintRole::Close:
		case HintRole::Destructive:
			p.drawLine(QPointF(-.7,-.7), QPointF(.7,.7));
			p.drawLine(QPointF(-.7,.7), QPointF(.7,-.7));
			break;
		default: break;
		}
		p.restore();
		text.adjust(rtl ? 0 : inset, 0, rtl ? -inset : 0, 0);
	}
	const auto fm = p.fontMetrics();
	const auto prefix = label.left(matched);
	const auto x = text.x() + (text.width() - fm.horizontalAdvance(label)) / 2;
	const auto y = text.y() + (text.height() - fm.height()) / 2 + fm.ascent();
	p.setLayoutDirection(Qt::LeftToRight);
	if (matched > 0) {
		p.setPen(colors.prefix);
		p.drawText(x, y, prefix);
		p.drawLine(x, y + stroke, x + fm.horizontalAdvance(prefix) - stroke, y + stroke);
	}
	p.setPen(colors.ink);
	p.drawText(x + fm.horizontalAdvance(prefix), y, label.mid(matched));
	p.restore();
}

} // namespace Purple

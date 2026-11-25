#pragma once

#include <QtGui/QPainter>

namespace Purple {

inline void PaintHintBadge(QPainter &p, QRect rect, QString label, int matched) {
	p.save();
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(QPen(QColor(0xb8, 0x92, 0x00), 1));
	p.setBrush(QColor(0xff, 0xd8, 0x4a));
	p.drawRoundedRect(QRectF(rect).adjusted(.5, .5, -.5, -.5), 4, 4);
	label = label.toUpper();
	const auto ink = QColor(0x1c, 0x19, 0x17);
	if (matched > 0 && matched < label.size()) {
		const auto fm = p.fontMetrics();
		const auto left = label.left(matched);
		const auto right = label.mid(matched);
		const auto total = fm.horizontalAdvance(label);
		const auto leading = fm.horizontalAdvance(left);
		const auto x = rect.x() + (rect.width() - total) / 2;
		p.setPen(QColor(0xb9, 0x1c, 0x1c));
		p.drawText(QRect(x, rect.y(), leading, rect.height()), Qt::AlignLeft | Qt::AlignVCenter, left);
		p.setPen(ink);
		p.drawText(QRect(x + leading, rect.y(), total - leading, rect.height()), Qt::AlignLeft | Qt::AlignVCenter, right);
	} else {
		p.setPen(ink);
		p.drawText(rect, Qt::AlignCenter, label);
	}
	p.restore();
}

inline void PaintHydraCap(
		QPainter &p,
		QRect rect,
		QString label,
		QColor fill,
		QColor border,
		QColor ink,
		int radius) {
	p.save();
	p.setPen(border);
	p.setBrush(fill);
	p.drawRoundedRect(rect, radius, radius);
	p.setPen(ink);
	p.drawText(rect, Qt::AlignCenter, p.fontMetrics().elidedText(label, Qt::ElideRight, rect.width()));
	p.restore();
}

} // namespace Purple

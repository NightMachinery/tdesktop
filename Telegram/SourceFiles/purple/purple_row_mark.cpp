/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_row_mark.h"

#include "dialogs/dialogs_entry.h"
#include "history/history.h"
#include "purple/purple_gate.h"
#include "styles/style_dialogs.h"
#include "styles/style_dialogs_layout.h"
#include "styles/style_widgets.h"
#include "styles/style_window.h"
#include "ui/painter.h"

#include <algorithm>

#include <QtCore/QRectF>
#include <QtGui/QPainter>
#include <QtGui/QPen>

namespace Purple {
namespace {

[[nodiscard]] const style::color &RowMarkForeground(
		bool active,
		bool held) {
	return active
		? st::dialogsTextFgActive
		: held
		? st::boxTextFgGood
		: st::dialogsUnreadBg;
}

} // namespace

RowMark RowMarkFor(not_null<Dialogs::Entry*> entry) {
	auto result = RowMark();
	if (const auto history = entry->asHistory()) {
		const auto temporary = history->purpleTemporary();
		result.from = temporary.from;
		result.until = temporary.until;
		result.held = temporary.held;
	}
	if (result.until) {
		switch (RecentMarkStyle()) {
		case RecentStyle::Stripe:
			result.style = RowMarkStyle::Stripe;
			break;
		case RecentStyle::Timer:
			result.style = RowMarkStyle::Timer;
			break;
		case RecentStyle::None:
			break;
		}
	}
	return result;
}

void PaintRowMarkStripe(
		QPainter &p,
		const RowMark &mark,
		bool active,
		int topPadding,
		const QRect &geometry) {
	auto hq = PainterHighQualityEnabler(p);
	const auto width = st::lineWidth * 3;
	const auto radius = width / 2.;
	p.setPen(Qt::NoPen);
	p.setBrush(RowMarkForeground(active, mark.held));
	p.drawRoundedRect(
		QRectF(
			geometry.x(),
			geometry.y() + topPadding,
			width,
			geometry.height() - 2 * topPadding),
		radius,
		radius);
}

int PaintRowMarkTimer(
		QPainter &p,
		const RowMark &mark,
		bool active,
		crl::time now,
		int right,
		int top) {
	const auto total = mark.until - mark.from;
	if (total <= 0) {
		return 0;
	}
	const auto left = mark.until - now;
	const auto part = std::clamp(left / float64(total), 0., 1.);
	const auto size = st::dialogsUnreadHeight;
	const auto inner = QRectF(
		right - size,
		top,
		size,
		size).marginsRemoved({ 1.5, 1.5, 1.5, 1.5 });

	auto hq = PainterHighQualityEnabler(p);
	auto pen = QPen(RowMarkForeground(active, mark.held)->c);
	pen.setWidthF(st::lineWidth * 1.5);
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	p.drawArc(inner, 90 * 16, int(part * 360 * 16));
	return size + st::dialogsUnreadPadding;
}

} // namespace Purple

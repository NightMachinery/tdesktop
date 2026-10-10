/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "crl/crl_time.h"

class QPainter;
class QRect;

namespace Dialogs {
class Entry;
}

namespace Purple {

enum class RowMarkStyle {
	None,
	Stripe,
	Timer,
};

struct RowMark {
	crl::time from = 0;
	crl::time until = 0;
	bool held = false;
	RowMarkStyle style = RowMarkStyle::None;
};

[[nodiscard]] RowMark RowMarkFor(not_null<Dialogs::Entry*> entry);

void PaintRowMarkStripe(
	QPainter &p,
	const RowMark &mark,
	bool active,
	int topPadding,
	const QRect &geometry);
[[nodiscard]] int PaintRowMarkTimer(
	QPainter &p,
	const RowMark &mark,
	bool active,
	crl::time now,
	int right,
	int top);

} // namespace Purple

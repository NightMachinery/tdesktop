#include "purple/purple_privacy_canvas.h"

#include <QtGui/QFontMetrics>
#include <QtGui/QLinearGradient>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QRegion>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Purple::PrivacyVisual {
namespace {

using PrivacyWeather::DailyWeather;
using PrivacyWeather::HourlyWeather;

struct Colors {
	QColor sky;
	QColor horizon;
	QColor hills;
	QColor ground;
	QColor panel;
	QColor ink;
	QColor muted;
	QColor rule;
	QColor accent;
	QColor symbol;
	QColor light;
};

Colors Palette(bool dark) {
	return dark
		? Colors{ 0x171d34, 0x484557, 0x46465f, 0x2e344a, 0x24283a,
			0xf9f4ee, 0xcec9da, 0x53566c, 0xd1b4f1, 0xd2d0e0, 0xffdc9f }
		: Colors{ 0xd1dedf, 0xb8ced1, 0xa2bbc0, 0x92aeb2, 0xf5f8f7,
			0x1d3544, 0x425863, 0xb4c5c7, 0x365f8d, 0x526b78, 0xb87843 };
}

QFont Font(QFont base, int size, QFont::Weight weight = QFont::Normal) {
	base.setPixelSize(std::max(1, size));
	base.setWeight(weight);
	base.setFeature(QFont::Tag("tnum"), 1);
	return base;
}

void Text(QPainter &p, const QRect &rect, const QString &text, const QFont &font,
		QColor color, Qt::Alignment align = Qt::AlignLeft | Qt::AlignVCenter) {
	if (rect.isEmpty()) {
		return;
	}
	p.setFont(font);
	p.setPen(color);
	const auto metrics = QFontMetrics(font);
	p.drawText(rect, align | Qt::TextSingleLine,
		metrics.horizontalAdvance(text) <= rect.width()
			? text : metrics.elidedText(text, Qt::ElideRight, rect.width()));
}

QString Time(const QLocale &locale, const QTime &time) {
	return locale.toString(time, QLocale::ShortFormat);
}

QString RelativeDay(qint64 days) {
	if (days == 1) {
		return QStringLiteral("Tomorrow");
	} else if (days == -1) {
		return QStringLiteral("Yesterday");
	} else if (days == 0) {
		return {};
	}
	return QStringLiteral("%1%2 days").arg(days > 0 ? QStringLiteral("+") : QString()).arg(days);
}

QString LocalizedTemperature(const Frame &frame, std::optional<int> celsius) {
	if (!celsius) {
		return QStringLiteral("–");
	}
	const auto value = frame.fahrenheit ? int(std::lround(*celsius * 1.8 + 32.)) : *celsius;
	return frame.locale.toString(value) + QChar(0xb0);
}

QString Temperature(const Frame &frame, std::optional<int> celsius) {
	return LocalizedTemperature(frame, celsius).replace(u'-', u'\u2212');
}

QString Chance(const Frame &frame, const HourlyWeather &hour) {
	return hour.precipitation
		? frame.locale.toString(*hour.precipitation) + frame.locale.percent()
		: QStringLiteral("–");
}

bool Rain(int code) {
	return (code >= 51 && code <= 67) || (code >= 80 && code <= 82)
		|| (code >= 95 && code <= 99);
}

void Moon(QPainter &p, QRectF rect, QColor color) {
	auto crescent = QPainterPath();
	crescent.arcMoveTo(rect, 90.);
	crescent.arcTo(rect, 90., 240.);
	const auto inner = rect.translated(rect.width() * std::sqrt(3.) / 4., -rect.height() / 4.);
	crescent.arcTo(inner, 270., -120.);
	crescent.closeSubpath();
	p.fillPath(crescent, color);
}

void Condition(QPainter &p, QRectF rect, int code, bool day, const Colors &colors) {
	p.save();
	p.translate(rect.topLeft());
	p.scale(rect.width(), rect.height());
	p.setPen(Qt::NoPen);
	if (code == 0 || code == 1) {
		const auto disc = QRectF(.1, .1, .8, .8);
		if (day) {
			p.setBrush(colors.light);
			p.drawEllipse(disc);
		} else {
			Moon(p, disc, colors.light);
		}
	} else if (code >= 0) {
		const auto raining = Rain(code);
		const auto snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
		const auto wet = raining || snow;
		const auto drop = wet ? .0 : .13;
		if (code == 2) {
			p.setBrush(colors.light);
			p.drawEllipse(QRectF(.48, .11, .38, .38));
		}
		auto cloud = QPainterPath();
		cloud.setFillRule(Qt::WindingFill);
		cloud.addRoundedRect(QRectF(.06, .34 + drop, .88, .30), .15, .15);
		cloud.addEllipse(QRectF(.20, .15 + drop, .43, .44));
		cloud.addEllipse(QRectF(.51, .24 + drop, .34, .34));
		p.fillPath(cloud, colors.symbol);
		if (raining) {
			p.setPen(QPen(colors.symbol, .060, Qt::SolidLine, Qt::RoundCap));
			for (const auto x : { .31, .52, .73 }) {
				p.drawLine(QPointF(x, .61), QPointF(x - .10, .87));
			}
		} else if (snow) {
			p.setBrush(colors.symbol);
			for (const auto x : { .27, .50, .73 }) {
				p.drawEllipse(QPointF(x, .80), .046, .046);
			}
		}
	}
	p.restore();
}

void Landscape(QPainter &p, const QRect &bounds, const QRegion &clockArea,
		int horizon, int unit, const Frame &frame, const Colors &colors) {
	const auto w = bounds.width();
	const auto h = bounds.height();
	const auto motifSize = std::clamp(w / 7, 3 * unit, 6 * unit);
	const auto motif = QRect(w - 2 * unit - motifSize, 2 * unit, motifSize, motifSize);
	const auto drawMotif = [&] {
		if (!clockArea.intersects(motif) && h >= 20 * unit) {
			Condition(p, motif, std::max(0, frame.motifCode), frame.day, colors);
		}
	};
	if (frame.flat) {
		p.fillRect(bounds, colors.sky);
		drawMotif();
		return;
	}
	auto sky = QLinearGradient(bounds.topLeft(), bounds.bottomRight());
	sky.setColorAt(0., colors.sky);
	sky.setColorAt(1., colors.horizon);
	p.fillRect(bounds, sky);
	if (!frame.day && horizon > unit) {
		auto star = colors.muted;
		p.setPen(Qt::NoPen);
		for (auto i = 0; i != 83; ++i) {
			const auto spread = ((i * 127 + 23) % 701) / 701.;
			const auto x = (1. - (1. - spread) * (1. - spread)) * w;
			const auto y = ((i * 89 + 7) % 379) * horizon / 379.;
			if (clockArea.contains(QPoint(int(x), int(y)))) {
				continue;
			}
			star.setAlpha(48 + (i % 5) * 20);
			p.setBrush(star);
			const auto radius = unit * ((i % 7 == 0) ? .075 : .045);
			p.drawEllipse(QPointF(x, y), radius, radius);
		}
	}
	const auto hills = [&](int y, int lift, QColor fill) {
		auto path = QPainterPath(QPointF(0, y));
		path.cubicTo(.17 * w, y + lift, .23 * w, y - lift, .40 * w, y);
		path.cubicTo(.57 * w, y + lift, .66 * w, y - 2 * lift, .81 * w, y);
		path.cubicTo(.90 * w, y + lift, .96 * w, y - lift, w, y);
		path.lineTo(w, h);
		path.lineTo(0, h);
		path.closeSubpath();
		p.fillPath(path, fill);
	};
	hills(horizon, unit, colors.hills);
	hills(horizon + unit, -unit, colors.ground);

	drawMotif();
}

struct Forecast {
	std::vector<HourlyWeather> hours;
	std::vector<DailyWeather> days;
};

Forecast Upcoming(const Frame &frame, int count) {
	auto result = Forecast();
	if (!frame.weather) {
		return result;
	}
	for (const auto &hour : frame.weather->hours) {
		if (hour.time > frame.local) {
			result.hours.push_back(hour);
			if (int(result.hours.size()) == count) break;
		}
	}
	const auto today = frame.local.toTimeZone(frame.weather->zone).date();
	for (const auto &day : frame.weather->days) {
		if (day.date > today) {
			result.days.push_back(day);
			if (result.days.size() == 3) break;
		}
	}
	return result;
}

struct Fonts {
	QFont caption;
	QFont label;
	QFont heading;
	QFont current;
};

struct ClockCell {
	QRect rect;
	QString label;
	QString time;
	QString relative;
	int divider = 0;
};

struct WeatherMetrics {
	int gap = 0;
	int inset = 0;
	int header = 0;
	int current = 0;
	int section = 0;
	int caption = 0;
	int icon = 0;
	int line = 0;
	int row = 0;
	int curveGap = 0;
	int curveHeight = 0;
	int precipitationGap = 0;
	int hourly = 0;
};

WeatherMetrics WeatherSizes(const Fonts &fonts, int unit, int extraSpacing) {
	const auto caption = QFontMetrics(fonts.caption).height();
	const auto section = QFontMetrics(fonts.heading).height();
	const auto line = QFontMetrics(fonts.label).height();
	const auto icon = unit * 7 / 5;
	const auto gap = std::max(1, unit / 4);
	const auto curveGap = gap + extraSpacing / 2;
	const auto curveHeight = unit + extraSpacing / 2;
	const auto precipitationGap = extraSpacing / 2;
	return {
		.gap = gap, .inset = unit, .header = std::max(caption, section),
		.current = QFontMetrics(fonts.current).height(), .section = section,
		.caption = caption, .icon = icon, .line = line,
		.row = icon + 2 * gap + extraSpacing * 5 / 8,
		.curveGap = curveGap, .curveHeight = curveHeight,
		.precipitationGap = precipitationGap,
		.hourly = section + 3 * caption + icon + line + curveHeight
			+ 3 * gap + 2 * curveGap + precipitationGap,
	};
}

void Hourly(QPainter &p, QRect rect, const Frame &frame,
		const std::vector<HourlyWeather> &hours, const Fonts &fonts,
		const WeatherMetrics &m, int unit, const Colors &c) {
	Text(p, { rect.x(), rect.y(), rect.width(), m.section },
		QStringLiteral("Hourly forecast"), fonts.heading, c.ink);
	const auto timeTop = rect.y() + m.section + m.gap;
	const auto iconTop = timeTop + m.caption + m.gap;
	const auto tempTop = iconTop + m.icon + m.gap;
	const auto curveTop = tempTop + m.line + m.curveGap;
	const auto rainTop = curveTop + m.curveHeight + m.curveGap;
	auto lows = std::numeric_limits<int>::max();
	auto highs = std::numeric_limits<int>::min();
	for (const auto &hour : hours) {
		if (hour.temperatureC) {
			lows = std::min(lows, *hour.temperatureC);
			highs = std::max(highs, *hour.temperatureC);
		}
	}
	auto previous = std::optional<QPointF>();
	const auto count = int(hours.size());
	for (auto i = 0; i != count; ++i) {
		const auto &hour = hours[i];
		const auto x = rect.x() + i * rect.width() / count;
		const auto w = rect.x() + (i + 1) * rect.width() / count - x;
		const auto center = x + w / 2;
		auto time = Time(frame.locale, hour.time.time());
		if (std::any_of(hours.begin(), hours.end(), [&](const HourlyWeather &other) {
			return hour.time.date() == other.time.date()
				&& hour.time.time() == other.time.time()
				&& hour.time.offsetFromUtc() != other.time.offsetFromUtc();
		})) {
			time += u' ' + hour.time.timeZoneAbbreviation();
		}
		Text(p, { x, timeTop, w, m.caption }, time, fonts.caption, c.muted, Qt::AlignCenter);
		const auto day = hour.time.time().hour() >= 7 && hour.time.time().hour() < 19;
		Condition(p, { qreal(center - m.icon / 2), qreal(iconTop), qreal(m.icon), qreal(m.icon) },
			hour.code, day, c);
		Text(p, { x, tempTop, w, m.line }, Temperature(frame, hour.temperatureC),
			fonts.label, c.ink, Qt::AlignCenter);
		Text(p, { x, rainTop, w, m.caption }, Chance(frame, hour), fonts.caption, c.muted, Qt::AlignCenter);
		if (hour.temperatureC) {
			const auto span = std::max(1, highs - lows);
			const auto mid = (lows + highs) / 2.;
			const auto point = QPointF(center,
				curveTop + m.curveHeight * (.5 - .63 * (*hour.temperatureC - mid) / span));
			p.setPen(QPen(c.accent, std::max(1, unit / 12), Qt::SolidLine, Qt::RoundCap));
			if (previous) p.drawLine(*previous, point);
			p.setPen(Qt::NoPen);
			p.setBrush(c.accent);
			p.drawEllipse(point, unit / 11., unit / 11.);
			previous = point;
		} else {
			previous.reset();
		}
	}
	Text(p, { rect.x(), rainTop + m.caption + m.precipitationGap, rect.width(), m.caption },
		QStringLiteral("Chance of precipitation"), fonts.caption, c.muted);
}

void Daily(QPainter &p, QRect rect, const Frame &frame,
		const std::vector<DailyWeather> &days, const Fonts &fonts,
		const WeatherMetrics &m, int unit, const Colors &c) {
	Text(p, { rect.x(), rect.y(), rect.width(), m.section },
		QStringLiteral("Next three days"), fonts.heading, c.ink);
	const auto fm = QFontMetrics(fonts.label);
	auto nameW = 0;
	auto lowW = fm.horizontalAdvance(LocalizedTemperature(frame, -10));
	auto highW = lowW;
	auto minimum = std::numeric_limits<int>::max();
	auto maximum = std::numeric_limits<int>::min();
	for (const auto &day : days) {
		nameW = std::max(nameW, fm.horizontalAdvance(frame.locale.dayName(day.date.dayOfWeek(), QLocale::ShortFormat)));
		lowW = std::max(lowW, fm.horizontalAdvance(Temperature(frame, day.lowC)));
		highW = std::max(highW, fm.horizontalAdvance(Temperature(frame, day.highC)));
		for (const auto temp : { day.lowC, day.highC }) {
			if (temp) {
				minimum = std::min(minimum, *temp);
				maximum = std::max(maximum, *temp);
			}
		}
	}
	nameW = std::min(nameW, rect.width() / 4);
	const auto iconX = rect.x() + nameW + m.gap;
	const auto lowX = iconX + m.icon + m.gap;
	const auto highX = rect.x() + rect.width() - highW;
	const auto start = lowX + lowW + m.gap;
	const auto finish = highX - m.gap;
	const auto y0 = rect.y() + m.section + m.gap;
	for (auto i = 0; i != int(days.size()); ++i) {
		const auto &day = days[i];
		const auto y = y0 + i * m.row;
		const auto mid = y + m.row / 2;
		Text(p, { rect.x(), y, nameW, m.row }, frame.locale.dayName(day.date.dayOfWeek(), QLocale::ShortFormat),
			fonts.label, c.ink);
		Condition(p, { qreal(iconX), qreal(mid - m.icon / 2), qreal(m.icon), qreal(m.icon) }, day.code, true, c);
		Text(p, { lowX, y, lowW, m.row }, Temperature(frame, day.lowC),
			fonts.label, c.muted, Qt::AlignRight | Qt::AlignVCenter);
		Text(p, { highX, y, highW, m.row }, Temperature(frame, day.highC),
			fonts.label, c.ink, Qt::AlignRight | Qt::AlignVCenter);
		if (finish > start) {
			p.setPen(QPen(c.rule, std::max(1, unit / 8), Qt::SolidLine, Qt::RoundCap));
			p.drawLine(start, mid, finish, mid);
			if (day.lowC && day.highC) {
				const auto point = [&](int t) {
					return (minimum == maximum) ? (start + finish) / 2.
						: start + (finish - start) * (t - minimum) / qreal(maximum - minimum);
				};
				const auto from = QPointF(point(*day.lowC), mid);
				const auto to = QPointF(point(*day.highC), mid);
				const auto width = std::max(2, unit / 5);
				p.setPen(QPen(c.accent, width, Qt::SolidLine, Qt::RoundCap));
				if (from == to) p.drawPoint(from);
				else p.drawLine(from, to);
			}
		}
	}
}

} // namespace

Canvas::Canvas(QWidget *parent) : QWidget(parent) {
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setFocusPolicy(Qt::NoFocus);
	setAccessibleName(QStringLiteral("Privacy screen"));
}

void Canvas::setFrame(Frame frame) {
	_frame = std::move(frame);
	auto description = QStringList{ _frame.locale.toString(_frame.local.date(), QLocale::LongFormat),
		Time(_frame.locale, _frame.local.time()) };
	for (const auto &clock : _frame.clocks) {
		description.push_back(clock.label + u' ' + Time(_frame.locale, clock.time.time()) + u' '
			+ RelativeDay(clock.dayDifference));
	}
	if (const auto &weather = _frame.weather) {
		description.push_back(QStringLiteral("Local weather %1 %2").arg(
			Temperature(_frame, weather->temperatureC), weather->condition));
		const auto upcoming = Upcoming(_frame, 6);
		for (const auto &hour : upcoming.hours) {
			description.push_back(QStringLiteral("%1 %2 %3, chance of precipitation %4").arg(
				Time(_frame.locale, hour.time.time()), hour.condition,
				Temperature(_frame, hour.temperatureC), Chance(_frame, hour)));
		}
		for (const auto &day : upcoming.days) {
			description.push_back(QStringLiteral("%1 %2, low %3 high %4").arg(
				_frame.locale.dayName(day.date.dayOfWeek(), QLocale::ShortFormat), day.condition,
				Temperature(_frame, day.lowC), Temperature(_frame, day.highC)));
		}
	}
	if (!_frame.update.isEmpty()) description.push_back(_frame.update);
	setAccessibleDescription(description.join(u'\n'));
	update();
}

void Canvas::paintEvent(QPaintEvent *) {
	auto p = QPainter(this);
	p.setRenderHint(QPainter::Antialiasing);
	const auto c = Palette(_frame.dark);
	const auto base = font();
	const auto unit = std::max(1, QFontMetrics(base).height());
	const auto outer = std::clamp(width() / 24, unit * 3 / 4, 3 * unit);
	const auto edge = _frame.controlsTop > 0 ? std::min(height(), _frame.controlsTop) : height();
	const auto available = std::max(0, edge - unit);
	const auto w = width() - 2 * outer;
	const auto wide = w >= 35 * unit;
	const auto compact = available < 24 * unit;
	const auto extraSpacing = wide
		? std::clamp((available - 30 * unit) / 12, 0, unit) : 0;
	const auto fonts = Fonts{ Font(base, unit * 4 / 5), Font(base, unit),
		Font(base, unit * 11 / 10, QFont::DemiBold), Font(base, unit * 2, QFont::DemiBold) };
	const auto metrics = WeatherSizes(fonts, unit, extraSpacing);
	const auto f = Upcoming(_frame, wide ? 6 : 3);
	const auto forecast = _frame.weather.has_value();
	const auto visible = forecast || !_frame.update.isEmpty();
	const auto weatherTitle = QStringLiteral("Local weather");
	const auto inner = w - 2 * metrics.inset;
	const auto updateW = QFontMetrics(fonts.caption).horizontalAdvance(_frame.update);
	const auto titleW = QFontMetrics(fonts.heading).horizontalAdvance(weatherTitle);
	const auto stackHeader = updateW > 0 && titleW + metrics.gap + updateW > inner;
	const auto headerH = metrics.header + (stackHeader ? metrics.gap + metrics.caption : 0);
	const auto hourly = !compact && !f.hours.empty() ? metrics.hourly : 0;
	const auto daily = !compact && !f.days.empty()
		? metrics.section + metrics.gap + int(f.days.size()) * metrics.row : 0;
	const auto sections = wide ? std::max(hourly, daily)
		: hourly + daily + ((hourly && daily) ? 2 * metrics.gap : 0);
	const auto panelH = !visible ? 0
		: 2 * metrics.inset + headerH + (forecast ? metrics.current + metrics.gap : 0)
			+ (sections ? sections + 4 * metrics.gap : 0);
	const auto panelY = std::max(0, available - panelH);
	const auto skyHeight = std::max(0, visible ? panelY - unit / 2 : available);

	const auto clockLeft = outer + unit / 2;
	const auto groupWidth = std::max(0, w - unit);
	const auto clockWidth = wide ? std::min(groupWidth, 31 * unit) : groupWidth;
	const auto dateFont = Font(base, unit * 6 / 5, QFont::Medium);
	const auto clockDateH = QFontMetrics(dateFont).height();
	const auto date = _frame.locale.toString(_frame.local.date(), QLocale::LongFormat);
	const auto time = Time(_frame.locale, _frame.local.time());
	const auto zoneCaption = Font(base, unit * 4 / 5 + extraSpacing / 6);
	const auto zoneCaptionMetrics = QFontMetrics(zoneCaption);
	const auto zoneCaptionH = zoneCaptionMetrics.height();
	const auto zoneFont = Font(base, unit * 3 / 2 + extraSpacing * 3 / 8, QFont::Medium);
	const auto zoneTimeH = QFontMetrics(zoneFont).height();
	const auto zoneH = 2 * zoneCaptionH + zoneTimeH + metrics.gap;
	const auto reserveZone = !_frame.clocks.empty()
		&& skyHeight > clockDateH + 2 * unit + zoneH ? zoneH : 0;
	const auto timeLimit = reserveZone
		? (skyHeight - clockDateH - reserveZone - unit) * 3 / 4 : skyHeight / 2;
	const auto proposed = std::clamp(std::min(groupWidth / 5, timeLimit), 2 * unit, 9 * unit);
	const auto measured = QFontMetrics(Font(base, proposed, QFont::Light)).horizontalAdvance(time);
	const auto timeFont = Font(base, measured > groupWidth && measured > 0
		? proposed * groupWidth / measured : proposed, QFont::Light);
	const auto timeH = QFontMetrics(timeFont).height();
	const auto columns = std::clamp(clockWidth / std::max(1, 6 * unit), 1, 3);
	const auto maxRows = std::max(0, (skyHeight - clockDateH - timeH - unit) / zoneH);
	const auto clocks = std::min(int(_frame.clocks.size()), compact ? std::min(1, maxRows) : maxRows * columns);
	const auto rows = (clocks + columns - 1) / columns;
	const auto blockH = clockDateH + timeH + rows * zoneH;
	const auto top = std::max(unit / 2, int((skyHeight - blockH) * (visible ? .29 : .56)));
	auto clockArea = QRegion();
	const auto exclude = [&](QRect rect) {
		const auto margin = unit / 3;
		clockArea += rect.adjusted(-margin, -margin, margin, margin);
	};
	exclude({ clockLeft, top, std::min(groupWidth, QFontMetrics(dateFont).horizontalAdvance(date)), clockDateH });
	exclude({ clockLeft, top + clockDateH,
		std::min(groupWidth, QFontMetrics(timeFont).horizontalAdvance(time)), timeH });
	const auto pitch = clockWidth / columns;
	const auto pad = unit * 3 / 4 + extraSpacing / 2;
	const auto zoneTime = QFontMetrics(zoneFont);
	auto cells = std::vector<ClockCell>();
	cells.reserve(clocks);
	auto x = clockLeft;
	for (auto i = 0; i != clocks; ++i) {
		const auto column = i % columns;
		const auto y = top + clockDateH + timeH + (i / columns) * zoneH;
		const auto &clock = _frame.clocks[i];
		const auto divider = column ? x + pad : 0;
		x = column ? divider + pad : clockLeft;
		const auto localTime = Time(_frame.locale, clock.time.time());
		const auto relative = RelativeDay(clock.dayDifference);
		const auto zw = std::min(pitch - 2 * pad,
			std::max({ zoneCaptionMetrics.horizontalAdvance(clock.label),
				zoneCaptionMetrics.horizontalAdvance(relative),
				zoneTime.horizontalAdvance(localTime) }));
		const auto rect = QRect(x, y, zw, zoneH);
		cells.push_back({ rect, clock.label, localTime, relative, divider });
		exclude(rect);
		x += zw;
	}
	const auto horizon = std::clamp(visible ? panelY - unit / 4 : int(height() * .65), 0, height());
	Landscape(p, rect(), clockArea, horizon, unit, _frame, c);

	Text(p, { clockLeft, top, groupWidth, clockDateH }, date, dateFont, c.muted);
	Text(p, { clockLeft, top + clockDateH, groupWidth, timeH }, time, timeFont, c.ink);
	for (const auto &cell : cells) {
		const auto x = cell.rect.x();
		const auto y = cell.rect.y();
		const auto w = cell.rect.width();
		if (cell.divider) {
			p.setPen(c.rule);
			p.drawLine(cell.divider, y + metrics.gap, cell.divider, y + zoneH - metrics.gap);
		}
		Text(p, { x, y, w, zoneCaptionH }, cell.label, zoneCaption, c.muted);
		Text(p, { x, y + zoneCaptionH, w, zoneTimeH }, cell.time, zoneFont, c.ink);
		Text(p, { x, y + zoneCaptionH + zoneTimeH, w, zoneCaptionH }, cell.relative, zoneCaption, c.muted);
	}

	if (visible && w > 0) {
		p.setPen(Qt::NoPen);
		p.setBrush(c.panel);
		p.drawRoundedRect(QRect(outer, panelY, w, panelH), unit * .75, unit * .75);
		const auto x = outer + metrics.inset;
		const auto y = panelY + metrics.inset;
		const auto headingW = stackHeader ? inner : inner - updateW - metrics.gap;
		Text(p, { x, y, headingW, metrics.header }, weatherTitle, fonts.heading, c.ink);
		if (stackHeader) {
			Text(p, { x, y + metrics.header + metrics.gap, inner, metrics.caption },
				_frame.update, fonts.caption, c.muted);
		} else {
			Text(p, { x + inner - updateW, y, updateW, metrics.header }, _frame.update, fonts.caption,
				c.muted, Qt::AlignRight | Qt::AlignVCenter);
		}
		if (forecast) {
			const auto temp = Temperature(_frame, _frame.weather->temperatureC)
				+ (_frame.fahrenheit ? u'F' : u'C');
			const auto currentY = y + headerH + metrics.gap;
			const auto icon = metrics.current * 2 / 3;
			Condition(p, { qreal(x), qreal(currentY + (metrics.current - icon) / 2), qreal(icon), qreal(icon) },
				_frame.weather->code, _frame.day, c);
			const auto tempX = x + icon + metrics.gap;
			const auto tempW = QFontMetrics(fonts.current).horizontalAdvance(temp);
			Text(p, { tempX, currentY, tempW, metrics.current }, temp, fonts.current, c.ink);
			const auto conditionX = tempX + tempW + 2 * metrics.gap;
			Text(p, { conditionX, currentY, x + inner - conditionX, metrics.current },
				_frame.weather->condition, fonts.label, c.ink);
			if (sections) {
				const auto ruleY = currentY + metrics.current + 2 * metrics.gap - 1;
				p.setPen(c.rule);
				p.drawLine(x, ruleY, x + inner, ruleY);
				const auto sectionsY = ruleY + 2 * metrics.gap + 1;
				const auto split = wide && hourly && daily;
				const auto divider = x + inner * 57 / 100;
				const auto dividerGap = unit + extraSpacing / 2;
				const auto hourW = split ? divider - x - dividerGap : inner;
				if (hourly) Hourly(p, { x, sectionsY, hourW, hourly }, _frame, f.hours, fonts, metrics, unit, c);
				if (daily) {
					const auto dx = split ? divider + dividerGap : x;
					const auto dy = sectionsY + ((hourly && !wide) ? hourly + 2 * metrics.gap : 0);
					Daily(p, { dx, dy, x + inner - dx, daily }, _frame, f.days, fonts, metrics, unit, c);
				}
				if (split) {
					p.setPen(c.rule);
					p.drawLine(divider, sectionsY, divider, sectionsY + sections);
				}
			}
		}
	}
	if (!_frame.hint.isEmpty()) {
		Text(p, { outer, height() - metrics.caption - unit / 2, w, metrics.caption },
			_frame.hint, fonts.caption, _frame.dark ? c.muted : c.ink, Qt::AlignCenter);
	}
}

} // namespace Purple::PrivacyVisual

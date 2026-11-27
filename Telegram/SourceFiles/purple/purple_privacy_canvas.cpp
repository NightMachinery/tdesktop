#include "purple/purple_privacy_canvas.h"

#include <QtCore/QEvent>
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
		if (!frame.weather && !clockArea.intersects(motif) && h >= 20 * unit) {
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

struct Fonts {
	QFont caption;
	QFont label;
	QFont heading;
	QFont figure;
};

struct Metrics {
	int unit = 0;
	int gap = 0;
	int caption = 0;
	int label = 0;
	int heading = 0;
	int icon = 0;
};

struct Forecast {
	std::vector<HourlyWeather> hours;
	std::vector<DailyWeather> days;
};

constexpr auto kSecondsPerHour = 3600;
constexpr auto kFullHours = 24;
constexpr auto kCompactHours = 12;

Forecast Upcoming(const Frame &frame) {
	auto result = Forecast();
	if (!frame.weather) {
		return result;
	}
	const auto now = frame.local.toSecsSinceEpoch();
	for (const auto &hour : frame.weather->hours) {
		const auto ahead = hour.time.toSecsSinceEpoch() - now;
		if (hour.time.isValid() && ahead > 0
			&& ahead <= kFullHours * kSecondsPerHour) {
			result.hours.push_back(hour);
		}
	}
	std::stable_sort(result.hours.begin(), result.hours.end(),
		[](const auto &a, const auto &b) { return a.time < b.time; });
	result.hours.erase(std::unique(result.hours.begin(), result.hours.end(),
		[](const auto &a, const auto &b) { return a.time == b.time; }),
		result.hours.end());
	if (result.hours.size() > kFullHours) {
		result.hours.resize(kFullHours);
	}
	const auto today = frame.weather->zone.isValid()
		? frame.local.toTimeZone(frame.weather->zone).date()
		: frame.local.date();
	for (const auto &day : frame.weather->days) {
		const auto ahead = today.daysTo(day.date);
		if (ahead > 0 && ahead <= 3) {
			result.days.push_back(day);
		}
	}
	return result;
}

QString ShortHour(const Frame &frame, QTime time) {
	if (frame.locale.timeFormat(QLocale::ShortFormat).contains(u'a', Qt::CaseInsensitive)) {
		const auto hour = time.hour() % 12;
		return frame.locale.toString(hour ? hour : 12);
	}
	return frame.locale.toString(time, QStringLiteral("HH"));
}

QString Offset(const Frame &frame, int seconds) {
	const auto magnitude = std::abs(seconds) / 60;
	return QStringLiteral("UTC%1%2").arg(seconds < 0 ? u'−' : u'+').arg(
		frame.locale.toString(QTime(magnitude / 60, magnitude % 60), QStringLiteral("HH:mm")));
}

bool Repeated(const HourlyWeather &hour, const std::vector<HourlyWeather> &hours) {
	return std::any_of(hours.begin(), hours.end(), [&](const auto &other) {
		return hour.time.date() == other.time.date()
			&& hour.time.time() == other.time.time()
			&& hour.time.offsetFromUtc() != other.time.offsetFromUtc();
	});
}

QString HourTime(const Frame &frame, const HourlyWeather &hour, bool repeated) {
	auto result = Time(frame.locale, hour.time.time());
	if (repeated) {
		result += u' ' + Offset(frame, hour.time.offsetFromUtc());
	}
	return result;
}

struct RainLane {
	std::optional<int> peak;
	QString summary;
	bool show = false;
};

RainLane Precipitation(const Frame &frame, const std::vector<HourlyWeather> &hours) {
	auto result = RainLane();
	auto missing = false;
	for (auto i = 0; i != int(hours.size()); ++i) {
		const auto chance = hours[i].precipitation;
		missing = missing || !chance
			|| (i && hours[i - 1].time.secsTo(hours[i].time) > kSecondsPerHour);
		if (chance && (!result.peak || *chance > *hours[*result.peak].precipitation)) {
			result.peak = i;
		}
	}
	if (result.peak) {
		const auto &hour = hours[*result.peak];
		result.show = missing || *hour.precipitation > 0;
		if (result.show) {
			result.summary = frame.labels.peak(Chance(frame, hour),
				HourTime(frame, hour, Repeated(hour, hours)));
		}
	}
	return result;
}

int Width(const QFont &font, const QString &string) {
	return QFontMetrics(font).horizontalAdvance(string);
}

QFont FitHeight(QFont font, int limit) {
	const auto height = QFontMetrics(font).height();
	if (height > limit) {
		font.setPixelSize(std::max(1, font.pixelSize() * std::max(1, limit) / height));
		while (font.pixelSize() > 1 && QFontMetrics(font).height() > limit) {
			font.setPixelSize(font.pixelSize() - 1);
		}
	}
	return font;
}

struct Layout {
	Fonts fonts;
	Metrics m;
	Forecast forecast;
	RainLane rain;
	QString heading;
	QRect header;
	QRect panel;
	int titleHeight = 0;
	int dailyHeight = 0;
	int chartHeight = 0;
	int rainHeight = 0;
	int repeatedHeight = 0;
	bool stackedCredit = false;
	bool stackedRain = false;
	bool columns = false;
	bool icons = false;
};

Layout Measure(const Frame &frame, const QRect &bounds, const QFont &base) {
	auto result = Layout();
	const auto unit = std::max(1, QFontMetrics(base).height());
	const auto u = unit;
	const auto gap = std::max(2, u / 3);
	const auto outer = std::clamp(bounds.width() / 24, u, 3 * u);
	const auto edge = frame.controlsTop > 0
		? std::min(bounds.height(), frame.controlsTop) : bounds.height();
	const auto available = std::max(0, edge - 2 * gap);
	const auto w = std::max(0, bounds.width() - 2 * outer);
	result.fonts = { Font(base, 4 * u / 5), Font(base, u),
		Font(base, 3 * u / 2, QFont::DemiBold), Font(base, 7 * u / 2, QFont::Light) };
	const auto &fonts = result.fonts;
	result.m = { u, gap, QFontMetrics(fonts.caption).height(),
		QFontMetrics(fonts.label).height(), QFontMetrics(fonts.heading).height(), u * 4 / 3 };
	const auto &m = result.m;
	const auto tall = available >= 33 * u;
	const auto inner = std::max(0, w - 2 * u);
	const auto headerHeight = tall
		? std::clamp(available / 4, 9 * u, 15 * u)
		: std::max(6 * u, available * 27 / 100);
	const auto budget = available - headerHeight - 2 * u;
	result.header = { outer + gap, gap, std::max(0, w - 2 * gap), available - gap };
	auto all = Upcoming(frame);
	auto minorWidth = 0;
	for (const auto &hour : all.hours) {
		minorWidth = std::max(minorWidth, Width(fonts.caption, ShortHour(frame, hour.time.time())));
	}
	result.columns = inner >= 49 * u;
	const auto full = tall && inner >= (kFullHours + 1) * (minorWidth + u);
	const auto hours = full ? kFullHours : kCompactHours;
	const auto limit = frame.local.addSecs(hours * kSecondsPerHour);
	all.hours.erase(std::remove_if(all.hours.begin(), all.hours.end(), [&](const auto &h) {
		return h.time > limit;
	}), all.hours.end());
	if (int(all.hours.size()) > hours) {
		all.hours.resize(hours);
	}
	if (inner < 18 * u || (all.hours.size() > 1 && inner < 12 * (minorWidth + gap))
		|| budget < 5 * u) {
		all.hours.clear();
	}
	if (tall && !all.days.empty()) {
		result.dailyHeight = m.label + 2 * gap + (result.columns
			? m.icon + m.caption + 2 * m.label + 3 * gap
			: int(all.days.size()) * (m.icon + 2 * gap));
	} else {
		all.days.clear();
	}
	result.forecast = std::move(all);
	auto &f = result.forecast;
	if (f.hours.empty() && f.days.empty()) {
		return result;
	}
	if (!f.hours.empty()) {
		result.heading = frame.labels.hours(int(f.hours.size()));
		if (frame.weather->zone.isValid()) {
			const auto offset = frame.weather->zone.offsetFromUtc(frame.local.toUTC());
			if (offset != frame.local.offsetFromUtc()) {
				result.heading += QStringLiteral(" · ") + Offset(frame, offset);
			}
		}
	} else {
		result.heading = frame.labels.days(int(f.days.size()));
		result.dailyHeight -= m.label + 2 * gap;
	}
	result.stackedCredit = Width(fonts.heading, result.heading) + u
		+ Width(fonts.caption, frame.update) > inner;
	result.titleHeight = m.heading + (result.stackedCredit ? m.caption + gap : 0);
	result.rain = Precipitation(frame, f.hours);
	result.stackedRain = result.rain.show && Width(fonts.caption, frame.labels.chance)
		+ Width(fonts.caption, result.rain.summary) + u > inner;
	auto remaining = budget - result.titleHeight - gap;
	if (!f.hours.empty()) {
		result.icons = tall;
		if (std::any_of(f.hours.begin(), f.hours.end(), [&](const auto &hour) {
			return Repeated(hour, f.hours);
		})) {
			result.repeatedHeight = m.caption + gap;
		}
		auto axis = m.caption + 3 * gap + result.repeatedHeight
			+ (result.icons ? m.icon + gap : 0);
		const auto rainText = result.rain.show
			? (result.stackedRain ? 2 * m.caption + gap : m.caption) + 2 * gap : 0;
		result.rainHeight = result.rain.show ? (tall ? 3 : 2) * u : 0;
		if (remaining - axis - rainText - result.rainHeight - result.dailyHeight < 4 * u) {
			f.days.clear();
			result.dailyHeight = 0;
		}
		if (remaining - axis - rainText - result.rainHeight < 4 * u && result.icons) {
			result.icons = false;
			axis -= m.icon + gap;
		}
		if (remaining - axis - rainText - result.rainHeight < 4 * u) {
			result.rainHeight = 0;
		}
		remaining -= axis + rainText + result.rainHeight + result.dailyHeight;
		if (remaining < 4 * u) {
			f.hours.clear();
			return result;
		}
		result.chartHeight = remaining;
	} else if (result.dailyHeight > remaining) {
		f.days.clear();
		return result;
	}
	result.header.setHeight(headerHeight - 2 * gap);
	const auto panelHeight = f.hours.empty()
		? result.titleHeight + gap + result.dailyHeight + 2 * u
		: available - headerHeight;
	result.panel = { outer, headerHeight, w, panelHeight };
	return result;
}

QRect Visual(const QPainter &p, QRect outer, QRect rect) {
	if (p.layoutDirection() == Qt::RightToLeft) {
		rect.moveLeft(outer.left() + outer.right() - rect.right());
	}
	return rect;
}

double VisualX(const QPainter &p, QRect outer, double x) {
	return (p.layoutDirection() == Qt::RightToLeft)
		? outer.left() + outer.right() - x : x;
}

Qt::Alignment Leading(const QPainter &p) {
	return Qt::AlignVCenter | Qt::AlignAbsolute | (p.layoutDirection() == Qt::RightToLeft
		? Qt::AlignRight : Qt::AlignLeft);
}

Qt::Alignment Trailing(const QPainter &p) {
	return Qt::AlignVCenter | Qt::AlignAbsolute | (p.layoutDirection() == Qt::RightToLeft
		? Qt::AlignLeft : Qt::AlignRight);
}

struct Header {
	struct ClockCell {
		QString label;
		QString time;
		QString relative;
		QRect rect;
	};
	QRect date;
	QRect time;
	QFont timeFont;
	QFont zoneFont;
	QRect current;
	std::vector<ClockCell> clocks;
};

Header MeasureHeader(const Frame &frame, const Layout &g, const QFont &base) {
	auto result = Header();
	const auto &m = g.m;
	const auto bounds = g.header;
	const auto weather = frame.weather.has_value();
	const auto status = !frame.update.isEmpty() && g.panel.isEmpty();
	const auto currentW = weather || status ? bounds.width() * 2 / 5 : 0;
	const auto clockW = bounds.width() - currentW - (currentW ? 2 * m.gap : 0);
	const auto tall = bounds.height() > 10 * m.unit;
	const auto compact = g.panel.isEmpty() && bounds.width() < 24 * m.unit;
	const auto top = bounds.y() + (tall ? m.unit : m.gap);
	const auto dateH = m.label;
	const auto time = Time(frame.locale, frame.local.time());
	result.zoneFont = Font(base, 3 * m.unit / 2, QFont::Medium);
	const auto zoneH = QFontMetrics(result.zoneFont).height() + 2 * m.caption;
	const auto timeSize = std::min(tall ? 6 * m.unit : 5 * m.unit, bounds.height() / 2);
	const auto goal = Font(base, timeSize, QFont::Light);
	auto timeFont = Font(base, std::min(timeSize,
		timeSize * clockW / std::max(1, Width(goal, time))), QFont::Light);
	auto stackClocks = compact || (!g.columns && g.icons);
	if (!frame.clocks.empty()) {
		const auto &first = frame.clocks.front();
		const auto firstTime = Time(frame.locale, first.time.time());
		const auto firstW = std::max(Width(g.fonts.caption, first.label),
			Width(result.zoneFont, firstTime));
		stackClocks = stackClocks
			|| Width(timeFont, time) + 5 * m.gap + firstW > clockW;
		if (stackClocks) {
			timeFont = FitHeight(timeFont, bounds.bottom() - top - dateH - zoneH - m.gap);
		}
	}
	result.timeFont = timeFont;
	const auto timeH = QFontMetrics(result.timeFont).height();
	const auto x = bounds.x() + m.gap;
	const auto timeW = Width(result.timeFont, time);
	result.date = { x, top, clockW - m.gap, dateH };
	result.time = { x, top + dateH, timeW, timeH };
	const auto clockBottom = top + dateH + timeH;
	const auto end = bounds.x() + clockW;
	auto nextX = stackClocks ? x : x + timeW + 4 * m.gap;
	auto nextY = stackClocks ? clockBottom + m.gap
		: std::max(top + dateH, clockBottom - zoneH);
	for (const auto &clock : frame.clocks) {
		const auto local = Time(frame.locale, clock.time.time());
		const auto relative = RelativeDay(clock.dayDifference);
		const auto cellW = std::min(clockW - m.gap,
			std::max({ Width(g.fonts.caption, clock.label),
				Width(g.fonts.caption, relative), Width(result.zoneFont, local) }));
		if (nextX + cellW > end) {
			nextX = x;
			nextY = std::max(clockBottom, nextY + zoneH) + m.gap;
		}
		if (nextY + zoneH > bounds.bottom() && !result.clocks.empty()) {
			break;
		}
		result.clocks.push_back({ clock.label, local, relative,
			QRect(nextX, nextY, cellW, zoneH) });
		nextX += cellW + 3 * m.gap;
		if (compact) {
			break;
		}
	}
	result.current = { end + 2 * m.gap, top, currentW, std::max(0, bounds.bottom() - top) };
	return result;
}

QString CurrentTemperature(const Frame &frame) {
	return Temperature(frame, frame.weather->temperatureC) + (frame.fahrenheit ? u'F' : u'C');
}

void PaintHeader(QPainter &p, const Frame &frame, const Layout &g,
		const Header &header, const QFont &base, const Colors &c) {
	const auto &m = g.m;
	const auto &f = g.fonts;
	const auto text = [&](QRect rect, const QString &s, const QFont &font, QColor color) {
		Text(p, Visual(p, g.header, rect), s, font, color, Leading(p));
	};
	text(header.date, frame.locale.toString(frame.local.date(), QLocale::LongFormat), f.label, c.muted);
	text(header.time, Time(frame.locale, frame.local.time()), header.timeFont, c.ink);
	for (const auto &clock : header.clocks) {
		auto r = clock.rect;
		const auto zoneH = QFontMetrics(header.zoneFont).height();
		if (r.x() > header.time.x() && r.y() < header.time.bottom()) {
			p.setPen(c.rule);
			const auto divider = VisualX(p, g.header, r.x() - 2 * m.gap);
			p.drawLine(QPointF(divider, r.y()), QPointF(divider, r.bottom()));
		}
		const auto w = r.width();
		text({ r.x(), r.y(), w, m.caption }, clock.label, f.caption, c.muted);
		r.translate(0, m.caption);
		text({ r.x(), r.y(), w, zoneH }, clock.time, header.zoneFont, c.ink);
		r.translate(0, zoneH);
		text({ r.x(), r.y(), w, m.caption }, clock.relative, f.caption, c.muted);
	}
	auto r = header.current;
	if (r.isEmpty()) {
		return;
	}
	if (!frame.weather) {
		p.setFont(f.caption);
		p.setPen(c.muted);
		p.drawText(Visual(p, g.header, r), Leading(p) | Qt::TextWordWrap, frame.update);
		return;
	}
	text({ r.x(), r.y(), r.width(), m.label }, frame.labels.local, f.label, c.muted);
	r.translate(0, m.label + 2 * m.gap);
	auto icon = std::min(3 * m.unit, r.width() / 4);
	const auto temp = CurrentTemperature(frame);
	const auto goal = Width(f.figure, temp);
	auto figure = f.figure;
	if (goal + icon + m.gap > r.width()) {
		figure = Font(base, f.figure.pixelSize() * std::max(1, r.width() - icon - m.gap)
			/ std::max(1, goal), QFont::Light);
	}
	const auto statusHeight = g.panel.isEmpty() && !frame.update.isEmpty()
		? m.caption + m.gap : 0;
	const auto height = g.header.bottom() + 1 - r.y() - statusHeight;
	figure = FitHeight(figure, height);
	const auto condition = frame.weather->condition;
	const auto conditionW = Width(f.label, condition);
	const auto stack = icon + Width(figure, temp) + 4 * m.gap + conditionW > r.width();
	if (stack) {
		figure = FitHeight(figure, height - m.label);
	}
	const auto figureH = QFontMetrics(figure).height();
	icon = std::min(icon, figureH);
	Condition(p, Visual(p, g.header, QRect(r.x(), r.y() + (figureH - icon) / 2, icon, icon)),
		frame.weather->code, frame.weather->isDay.value_or(frame.day), c);
	const auto tempX = r.x() + icon + m.gap;
	const auto tempW = Width(figure, temp);
	text({ tempX, r.y(), tempW, figureH }, temp, figure, c.ink);
	if (stack) {
		text({ r.x(), r.y() + figureH, r.width(), m.label }, condition, f.label, c.ink);
		r.translate(0, m.label);
	} else {
		text({ tempX + tempW + 2 * m.gap, r.y(), conditionW + m.gap, figureH },
			condition, f.label, c.ink);
	}
	if (g.panel.isEmpty() && !frame.update.isEmpty()) {
		p.setFont(f.caption);
		p.setPen(c.muted);
		const auto y = r.y() + figureH + m.gap;
		const auto room = g.header.bottom() - y + 1;
		if (room > 0) {
			p.drawText(Visual(p, g.header, QRect(r.x(), y, r.width(), room)),
				Leading(p) | Qt::TextWordWrap, frame.update);
		}
	}
}

struct AxisLabel {
	QString full;
	QString shortTime;
	bool midnight = false;
	bool repeated = false;
	bool major = false;
	bool minor = false;
	int row = 0;
};

std::vector<AxisLabel> AxisLabels(const Frame &frame, const std::vector<HourlyWeather> &hours) {
	auto result = std::vector<AxisLabel>();
	result.reserve(hours.size());
	for (auto i = 0; i != int(hours.size()); ++i) {
		const auto &hour = hours[i];
		const auto previousDate = i ? hours[i - 1].time.date()
			: frame.local.toTimeZone(hour.time.timeZone()).date();
		const auto midnight = hour.time.date() != previousDate;
		const auto repeated = Repeated(hour, hours);
		const auto day = midnight
			? frame.locale.dayName(hour.time.date().dayOfWeek(), QLocale::ShortFormat) + u' '
			: QString();
		result.push_back({ day + HourTime(frame, hour, repeated),
			ShortHour(frame, hour.time.time()), midnight, repeated });
	}
	return result;
}

void PaintTimeline(QPainter &p, QRect rect, int y, const Frame &frame,
		const Layout &g, const Colors &c) {
	const auto &m = g.m;
	const auto &f = g.fonts;
	const auto &hours = g.forecast.hours;
	const auto count = int(hours.size());
	auto labels = AxisLabels(frame, hours);
	const auto endpoints = std::max(Width(f.caption, labels.front().full),
		Width(f.caption, labels.back().full));
	const auto inset = std::min(rect.width() / 6, std::max(m.unit, endpoints / 2 + m.gap));
	const auto plot = rect.adjusted(inset, 0, -inset, 0);
	const auto secs = hours.front().time.secsTo(hours.back().time);
	const auto timeX = [&](qint64 offset) {
		return VisualX(p, rect, secs ? plot.left() + (plot.width() - 1)
			* offset / double(secs) : plot.center().x());
	};
	const auto x = [&](int i) { return timeX(hours.front().time.secsTo(hours[i].time)); };
	const auto chartTop = y + m.label;
	const auto chartBottom = y + g.chartHeight;
	p.setPen(c.rule);
	p.drawLine(plot.left(), chartBottom, plot.right(), chartBottom);
	auto used = QRegion();
	auto majorCount = 0;
	const auto fitLabel = [&](int i, bool major) {
		auto &label = labels[i];
		if (label.major || label.minor || (!major && (label.repeated || label.midnight))) {
			return;
		}
		const auto w = major ? std::max(Width(f.caption, label.full),
			Width(f.label, Temperature(frame, hours[i].temperatureC))) : Width(f.caption, label.shortTime);
		auto target = QRect(int(std::lround(x(i))) - w / 2 - m.gap / 2, 0, w + m.gap, 1);
		if (major && label.repeated && used.intersects(target)) {
			target.moveTop(1);
		}
		if (!used.intersects(target) && target.left() >= rect.left() && target.right() <= rect.right()) {
			used += target;
			label.major = major;
			label.minor = !major;
			label.row = target.y();
			majorCount += major;
		}
	};
	fitLabel(0, true);
	fitLabel(count - 1, true);
	for (auto i = 1; i + 1 < count; ++i) {
		if (labels[i].midnight || labels[i].repeated) {
			fitLabel(i, true);
		}
	}
	for (auto i = 3; i + 1 < count; i += 4) {
		if (count > kCompactHours || majorCount < 4) {
			fitLabel(i, true);
		}
	}
	for (auto i = 0; i != count; ++i) {
		fitLabel(i, false);
	}
	auto minimum = std::numeric_limits<int>::max();
	auto maximum = std::numeric_limits<int>::min();
	for (const auto &hour : hours) {
		if (hour.temperatureC) {
			minimum = std::min(minimum, *hour.temperatureC);
			maximum = std::max(maximum, *hour.temperatureC);
		}
	}
	auto points = std::vector<QPointF>();
	const auto flush = [&] {
		if (points.size() > 1) {
			auto shade = c.accent;
			shade.setAlpha(20);
			auto fill = QPainterPath(QPointF(points.front().x(), chartBottom));
			for (const auto &point : points) fill.lineTo(point);
			fill.lineTo(points.back().x(), chartBottom);
			fill.closeSubpath();
			p.fillPath(fill, shade);
			p.setPen(QPen(c.accent, std::max(2., m.unit / 7.), Qt::SolidLine, Qt::RoundCap));
			p.drawPolyline(points.data(), int(points.size()));
		}
		points.clear();
	};
	const auto point = [&](int i) {
		const auto mid = (minimum + maximum) / 2.;
		const auto span = std::max(4, maximum - minimum);
		const auto fraction = .5 - .75 * (*hours[i].temperatureC - mid) / span;
		return QPointF(x(i), chartTop + (chartBottom - chartTop) * fraction);
	};
	for (auto i = 0; i != count; ++i) {
		if (!hours[i].temperatureC || (i && hours[i - 1].time.secsTo(hours[i].time) != kSecondsPerHour)) {
			flush();
		}
		if (hours[i].temperatureC) {
			points.push_back(point(i));
		}
		if (labels[i].midnight) {
			p.setPen(c.rule);
			p.drawLine(QPointF(x(i), chartTop), QPointF(x(i), chartBottom));
		}
	}
	flush();
	const auto axisTop = chartBottom + 2 * m.gap;
	for (auto i = 0; i != count; ++i) {
		const auto &hour = hours[i];
		const auto &label = labels[i];
		const auto center = int(std::lround(x(i)));
		p.setPen(c.rule);
		p.drawLine(center, chartBottom, center, chartBottom + m.gap);
		if (label.major || label.minor) {
			const auto time = label.major ? label.full : label.shortTime;
			const auto w = Width(f.caption, time) + 2;
			Text(p, { center - w / 2, axisTop + label.row * g.repeatedHeight, w, m.caption }, time,
				f.caption, label.major ? c.ink : c.muted, Qt::AlignCenter);
		} else if (label.midnight) {
			const auto name = frame.locale.dayName(hour.time.date().dayOfWeek(), QLocale::ShortFormat);
			const auto w = Width(f.caption, name) + m.gap;
			Text(p, { std::clamp(center - w / 2, rect.left(), rect.right() + 1 - w),
				y, w, m.caption }, name, f.caption, c.muted, Qt::AlignCenter);
		}
		if (hour.temperatureC) {
			const auto pos = point(i);
			const auto r = label.major ? m.unit / 5. : m.unit / 10.;
			p.setPen(Qt::NoPen);
			p.setBrush(c.accent);
			p.drawEllipse(pos, r, r);
			if (label.major) {
				const auto temp = Temperature(frame, hour.temperatureC);
				const auto w = Width(f.label, temp) + 2;
				Text(p, { center - w / 2, int(pos.y()) - m.label - m.gap, w, m.label },
					temp, f.label, c.ink, Qt::AlignCenter);
			}
		}
		if (g.icons && label.major) {
			const auto h = hour.time.time().hour();
			Condition(p, QRect(center - m.icon / 2,
				axisTop + m.caption + m.gap + g.repeatedHeight,
				m.icon, m.icon), hour.code, h >= 7 && h < 19, c);
		}
	}
	if (!g.rain.show) {
		return;
	}
	const auto rainY = axisTop + m.caption + 2 * m.gap + g.repeatedHeight
		+ (g.icons ? m.icon + m.gap : 0);
	const auto rainTitleY = rainY + (g.stackedRain ? m.caption + m.gap : 0);
	if (g.rainHeight || !g.stackedRain) {
		const auto w = g.stackedRain ? rect.width() : Width(f.caption, frame.labels.chance);
		Text(p, Visual(p, rect, QRect(rect.x(), rainY, w, m.caption)),
			frame.labels.chance, f.caption, c.muted, Leading(p));
	}
	const auto summaryW = g.stackedRain ? rect.width() : Width(f.caption, g.rain.summary);
	Text(p, Visual(p, rect, QRect(rect.right() + 1 - summaryW, rainTitleY, summaryW, m.caption)),
		g.rain.summary, f.caption, c.ink, Trailing(p));
	if (g.rainHeight) {
		const auto bottom = rainTitleY + m.caption + g.rainHeight;
		const auto barW = std::min(2 * m.unit, std::max(2, plot.width() / count / 2));
		p.setPen(c.rule);
		p.drawLine(plot.left(), bottom, plot.right(), bottom);
		const auto dash = [&](double center) {
			p.setPen(c.muted);
			p.drawLine(QPointF(center - barW / 4., bottom - m.gap),
				QPointF(center + barW / 4., bottom - m.gap));
		};
		for (auto i = 0; i != count; ++i) {
			const auto chance = hours[i].precipitation;
			if (i) {
				const auto from = hours.front().time.secsTo(hours[i - 1].time);
				const auto to = hours.front().time.secsTo(hours[i].time);
				for (auto offset = from + kSecondsPerHour; offset < to; offset += kSecondsPerHour) {
					dash(timeX(offset));
				}
			}
			if (!chance) {
				dash(x(i));
			} else if (*chance > 0) {
				const auto h = std::max(2., (g.rainHeight - m.gap) * *chance / 100.);
				p.setPen(Qt::NoPen);
				p.setBrush(c.accent);
				p.drawRoundedRect(QRectF(x(i) - barW / 2., bottom - h, barW, h), 2., 2.);
			}
		}
	}
}

void PaintDays(QPainter &p, QRect rect, const Frame &frame, const Layout &g,
		const Colors &c, bool heading) {
	const auto &m = g.m;
	const auto &f = g.fonts;
	const auto &days = g.forecast.days;
	const auto count = int(days.size());
	if (heading) {
		p.setPen(c.rule);
		p.drawLine(rect.left(), rect.top(), rect.right(), rect.top());
		rect.setTop(rect.top() + m.gap);
		Text(p, rect.adjusted(0, 0, 0, m.label - rect.height()), frame.labels.days(count),
			f.label, c.ink, Leading(p));
		rect.setTop(rect.top() + m.label + m.gap);
	}
	auto textW = 0;
	auto dayW = 0;
	auto minimum = std::numeric_limits<int>::max();
	auto maximum = std::numeric_limits<int>::min();
	for (const auto &day : days) {
		dayW = std::max(dayW, Width(f.label, frame.locale.dayName(day.date.dayOfWeek())));
		for (const auto t : { day.lowC, day.highC }) {
			textW = std::max(textW, Width(f.caption, Temperature(frame, t)) + 2);
			if (t) {
				minimum = std::min(minimum, *t);
				maximum = std::max(maximum, *t);
			}
		}
	}
	const auto row = m.icon + 2 * m.gap;
	const auto colW = rect.width() / count;
	for (auto i = 0; i != count; ++i) {
		const auto &day = days[i];
		const auto name = frame.locale.dayName(day.date.dayOfWeek());
		const auto x = rect.x() + (g.columns ? i * colW : 0);
		const auto y = rect.y() + (g.columns ? 0 : i * row);
		const auto w = g.columns ? colW - 2 * m.gap : rect.width();
		const auto iconX = g.columns ? x : x + std::min(dayW, w / 4) + 2 * m.gap;
		const auto labelX = g.columns ? iconX + m.icon + m.gap : x;
		const auto labelW = g.columns ? w - m.icon - m.gap : iconX - x - m.gap;
		Condition(p, Visual(p, rect, QRect(iconX, y, m.icon, m.icon)), day.code, true, c);
		Text(p, Visual(p, rect, QRect(labelX, y, labelW, m.label)), name, f.label, c.ink, Leading(p));
		const auto detailX = iconX + m.icon + m.gap;
		const auto detailW = g.columns ? x + w - detailX : x + w / 2 - detailX;
		Text(p, Visual(p, rect, QRect(detailX, y + (g.columns ? m.label : 0), detailW, m.caption)),
			day.condition, f.caption, c.muted, Leading(p));
		const auto trackY = y + (g.columns ? m.label + m.caption + 2 * m.gap + m.label / 2 : m.label / 2);
		const auto track = QRect(g.columns ? x : x + w / 2, trackY - m.label / 2,
			g.columns ? w : w - w / 2, m.label);
		const auto left = track.left() + textW + m.gap;
		const auto right = track.right() - textW - m.gap;
		const auto paintValue = [&](int edge, std::optional<int> value, bool low) {
			const auto pos = low ? edge - m.gap - textW : edge + m.gap;
			const auto label = Visual(p, rect, QRect(pos, track.y(), textW, track.height()));
			p.fillRect(label, c.panel);
			Text(p, label, Temperature(frame, value), f.caption,
				low ? c.muted : c.ink, low ? Trailing(p) : Leading(p));
		};
		if (left > right) {
			Text(p, Visual(p, rect, track), Temperature(frame, day.lowC)
				+ QStringLiteral(" / ") + Temperature(frame, day.highC),
				f.caption, c.ink, Trailing(p));
			continue;
		}
		const auto pos = [&](int value) {
			return (minimum == maximum) ? (left + right) / 2
				: left + (right - left) * (value - minimum) / (maximum - minimum);
		};
		const auto low = day.lowC ? pos(*day.lowC) : left;
		const auto high = day.highC ? pos(*day.highC) : right;
		p.setPen(QPen(c.rule, std::max(1., m.unit / 5.), Qt::SolidLine, Qt::RoundCap));
		p.drawLine(QPointF(VisualX(p, rect, left), trackY), QPointF(VisualX(p, rect, right), trackY));
		p.setPen(QPen(c.accent, std::max(2., m.unit / 4.), Qt::SolidLine, Qt::RoundCap));
		if (day.lowC && day.highC && low != high) {
			p.drawLine(QPointF(VisualX(p, rect, low), trackY), QPointF(VisualX(p, rect, high), trackY));
		} else if (day.lowC || day.highC) {
			p.drawPoint(QPointF(VisualX(p, rect, day.lowC ? low : high), trackY));
		}
		paintValue(low, day.lowC, true);
		paintValue(high, day.highC, false);
		if (g.columns && i) {
			p.setPen(c.rule);
			const auto divider = VisualX(p, rect, x - m.gap);
			p.drawLine(QPointF(divider, y), QPointF(divider, track.bottom()));
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
	refreshDescription();
	update();
}

void Canvas::resizeEvent(QResizeEvent *event) {
	QWidget::resizeEvent(event);
	refreshDescription();
}

void Canvas::changeEvent(QEvent *event) {
	QWidget::changeEvent(event);
	if (event->type() == QEvent::FontChange || event->type() == QEvent::LayoutDirectionChange) {
		refreshDescription();
	}
}

void Canvas::refreshDescription() {
	const auto g = Measure(_frame, rect(), font());
	const auto header = MeasureHeader(_frame, g, font());
	auto description = QStringList{ _frame.locale.toString(_frame.local.date(), QLocale::LongFormat),
		Time(_frame.locale, _frame.local.time()) };
	for (const auto &clock : header.clocks) {
		description.push_back(clock.label + u' ' + clock.time + u' ' + clock.relative);
	}
	if (_frame.weather) {
		description.push_back(_frame.labels.local + u' ' + CurrentTemperature(_frame)
			+ u' ' + _frame.weather->condition);
		if (!g.panel.isEmpty()) {
			description.push_back(g.heading);
			const auto &hours = g.forecast.hours;
			for (const auto &hour : hours) {
				description.push_back(QStringLiteral("%1 %2 %3, chance of precipitation %4").arg(
					_frame.locale.toString(hour.time.date(), QLocale::ShortFormat)
						+ u' ' + HourTime(_frame, hour, Repeated(hour, hours)),
					hour.condition, Temperature(_frame, hour.temperatureC), Chance(_frame, hour)));
			}
			if (!hours.empty() && !g.rain.peak) {
				description.push_back(_frame.labels.unavailable);
			} else if (!g.rain.summary.isEmpty()) {
				description.push_back(g.rain.summary);
			}
			if (!hours.empty() && !g.forecast.days.empty()) {
				description.push_back(_frame.labels.days(int(g.forecast.days.size())));
			}
			for (const auto &day : g.forecast.days) {
				description.push_back(QStringLiteral("%1 %2, low %3 high %4").arg(
					_frame.locale.dayName(day.date.dayOfWeek()), day.condition,
					Temperature(_frame, day.lowC), Temperature(_frame, day.highC)));
			}
		}
	}
	if (!_frame.update.isEmpty()) description.push_back(_frame.update);
	setAccessibleDescription(description.join(u'\n'));
}

void Canvas::paintEvent(QPaintEvent *) {
	auto p = QPainter(this);
	p.setRenderHint(QPainter::Antialiasing);
	if (_frame.locale.textDirection() == Qt::RightToLeft) {
		p.setLayoutDirection(Qt::RightToLeft);
	}
	const auto c = Palette(_frame.dark);
	const auto g = Measure(_frame, rect(), font());
	const auto &m = g.m;
	const auto &f = g.fonts;
	const auto header = MeasureHeader(_frame, g, font());
	auto clockArea = QRegion();
	for (const auto &clock : header.clocks) clockArea += Visual(p, g.header, clock.rect);
	clockArea += Visual(p, g.header, header.date);
	clockArea += Visual(p, g.header, header.time);
	clockArea += Visual(p, g.header, header.current);
	const auto horizon = g.panel.isEmpty() ? int(height() * .66) : g.panel.top() - m.gap;
	Landscape(p, rect(), clockArea, std::max(0, horizon), m.unit, _frame, c);
	PaintHeader(p, _frame, g, header, font(), c);

	if (!g.panel.isEmpty()) {
		p.setPen(Qt::NoPen);
		p.setBrush(c.panel);
		p.drawRoundedRect(g.panel, m.unit, m.unit);
		const auto inside = g.panel.adjusted(m.unit, m.unit, -m.unit, -m.unit);
		const auto titleW = g.stackedCredit ? inside.width()
			: std::max(0, inside.width() - Width(f.caption, _frame.update) - m.gap);
		Text(p, Visual(p, inside, QRect(inside.x(), inside.y(), titleW, m.heading)),
			g.heading, f.heading, c.ink, Leading(p));
		const auto creditY = inside.y() + (g.stackedCredit ? m.heading + m.gap : 0);
		Text(p, { inside.x(), creditY, inside.width(), g.stackedCredit ? m.caption : m.heading },
			_frame.update, f.caption, c.muted, g.stackedCredit ? Leading(p) : Trailing(p));
		const auto y = inside.y() + g.titleHeight + m.gap;
		const auto hours = !g.forecast.hours.empty();
		if (hours) {
			PaintTimeline(p, inside, y, _frame, g, c);
		}
		if (!g.forecast.days.empty()) {
			const auto start = hours ? inside.bottom() + 1 - g.dailyHeight : y;
			PaintDays(p, QRect(inside.x(), start, inside.width(), g.dailyHeight), _frame, g, c, hours);
		}
	}
	if (!_frame.hint.isEmpty()) {
		Text(p, { 0, height() - m.caption - m.unit / 2, width(), m.caption },
			_frame.hint, f.caption, _frame.dark ? c.muted : c.ink, Qt::AlignCenter);
	}
}

} // namespace Purple::PrivacyVisual

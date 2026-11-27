#include "purple/purple_privacy_canvas.h"

#include <QtCore/QDir>
#include <QtGui/QFontDatabase>
#include <QtWidgets/QApplication>

#include <cstdlib>
#include <iostream>

namespace {

using Purple::PrivacyVisual::Canvas;
using Purple::PrivacyVisual::Frame;
using Purple::PrivacyWeather::DescribeWmoCode;
using Purple::PrivacyWeather::WeatherSnapshot;

auto Checks = 0;

void Check(bool ok, const char *description) {
	++Checks;
	if (!ok) {
		std::cerr << description << '\n';
		std::exit(EXIT_FAILURE);
	}
}

Frame Fixture(bool dark, bool day, const QLocale &locale) {
	const auto date = QDate(2026, 11, 25);
	const auto clock = QTimeZone(QByteArray("Europe/Berlin"));
	const auto now = QDateTime(date, day ? QTime(9, 40) : QTime(22, 15), clock);
	auto weather = WeatherSnapshot{
		.location = {},
		.temperatureC = day ? 14 : 8,
		.condition = DescribeWmoCode(day ? 61 : 0),
		.valid = true,
		.isDay = day,
		.zone = clock,
		.hours = {},
		.days = {},
		.code = day ? 61 : 0,
	};
	const auto first = QDateTime(now.date(), QTime(now.time().hour(), 0), clock).addSecs(3600);
	for (auto i = 0; i != 24; ++i) {
		const auto code = day ? (i < 2 ? 61 : i == 3 ? 2 : 3) : (i < 3 ? 0 : 3);
		weather.hours.push_back({ first.addSecs(i * 3600), day ? 5 + std::abs(i - 13) / 2 : 3 + std::abs(i - 11) / 2,
			i == 0 || i > 8 ? 0 : 80 - std::abs(i - 4) * 16, DescribeWmoCode(code), code });
	}
	for (auto i = 0; i != 3; ++i) {
		const auto low = day ? (i == 0 ? 10 : i == 1 ? -10 : 9) : 3 + i;
		const auto high = day ? 18 + (i % 2) : 9 + i;
		const auto code = (day || i == 2) ? 61 : i == 0 ? 2 : 3;
		weather.days.push_back({ date.addDays(i + 1), low, high, DescribeWmoCode(code), code });
	}
	return {
		.local = now,
		.locale = locale,
		.clocks = Purple::PrivacyClocks::At(now, clock, {
			{ QStringLiteral("Asia/Tehran"), QStringLiteral("Tehran") },
			{ QStringLiteral("Asia/Tokyo"), QStringLiteral("Tokyo") },
			{ QStringLiteral("America/New_York"), QStringLiteral("New York") },
		}),
		.weather = std::move(weather),
		.update = day ? QStringLiteral("Updated 09:30 · Open-Meteo") : QStringLiteral("Updated 22:00 · Open-Meteo"),
		.hint = QStringLiteral("Press Esc or q to return"),
		.motifCode = day ? 61 : 0,
		.dark = dark,
		.day = day,
		.labels = {
			.local = QStringLiteral("Local weather"),
			.chance = QStringLiteral("Chance of precipitation"),
			.unavailable = QStringLiteral("Precipitation data unavailable"),
			.hours = [](int count) { return QStringLiteral("Next %1 hours").arg(count); },
			.days = [](int count) { return QStringLiteral("Next %1 days").arg(count); },
			.peak = [](const QString &chance, const QString &time) {
				return QStringLiteral("Peak %1 at %2").arg(chance, time);
			},
		},
	};
}

QString Render(const QDir &output, const QString &name, QSize size, Frame frame, int pixels) {
	auto canvas = Canvas();
	auto font = QFont(QStringLiteral("Open Sans"));
	font.setPixelSize(pixels);
	canvas.setFont(font);
	canvas.resize(size);
	frame.controlsTop = size.height() - 4 * pixels;
	canvas.setFrame(frame);
	canvas.show();
	QApplication::processEvents();
	const auto shot = canvas.grab();
	Check(!shot.isNull() && shot.deviceIndependentSize().toSize() == size, "production canvas fits viewport");
	Check(shot.save(output.filePath(name + QStringLiteral(".png"))), "save offscreen production canvas");
	if (name.startsWith(QStringLiteral("timeline-short"))) {
		const auto pixels = shot.toImage();
		const auto panel = frame.dark ? QColor(0x24283a) : QColor(0xf5f8f7);
		const auto ink = frame.dark ? QColor(0xf9f4ee) : QColor(0x1d3544);
		auto top = pixels.height() / 6;
		while (top < pixels.height() / 2
			&& pixels.pixelColor(pixels.width() / 2, top) != panel) {
			++top;
		}
		Check(top < pixels.height() / 2, "short fixture finds the top of the forecast panel");
		auto coveredInk = false;
		for (auto x = 6 * pixels.width() / 10; x < 9 * pixels.width() / 10; ++x) {
			coveredInk |= pixels.pixelColor(x, top - 1) == ink;
		}
		Check(!coveredInk, "current conditions clear the short forecast panel");
	}
	const auto desc = canvas.accessibleDescription();
	Check(desc.contains(frame.locale.toString(frame.local.time(), QLocale::ShortFormat)),
		"widget reports the locale-formatted time");
	Check(desc.contains(frame.locale.toString(frame.local.date(), QLocale::LongFormat)),
		"full date and year remain accessible");
	if (frame.weather) {
		Check(desc.contains(QStringLiteral("Open-Meteo")), "weather attribution is available");
		Check(!desc.contains(QStringLiteral("Private fixture town")), "weather location is private");
	}
	return desc;
}

} // namespace

int main(int argc, char **argv) {
	const auto app = QApplication(argc, argv);
	Check(argc == 3, "pass a render directory and the bundled font directory");
	const auto fonts = QDir(QString::fromLocal8Bit(argv[2]));
	for (const auto name : { "OpenSans-Regular.ttf", "OpenSans-SemiBold.ttf" }) {
		Check(QFontDatabase::addApplicationFont(fonts.filePath(QString::fromLatin1(name))) >= 0,
			"load the app’s bundled Open Sans");
	}
	const auto output = QDir(QString::fromLocal8Bit(argv[1]));
	Check(output.mkpath(QStringLiteral(".")), "create the offscreen output directory");
	const auto british = QLocale(QLocale::English, QLocale::UnitedKingdom);
	const auto american = QLocale(QLocale::English, QLocale::UnitedStates);
	for (const auto dark : { false, true }) {
		const auto theme = dark ? QStringLiteral("-dark") : QStringLiteral("-light");
		auto frame = Fixture(dark, !dark, british);
		frame.weather->location = QStringLiteral("Private fixture town");
		frame.flat = true;
		auto desc = Render(output, QStringLiteral("timeline-full") + theme, { 1512, 982 }, frame, 14);
		Check(desc.contains(QStringLiteral("Next 24 hours")), "full layout selects 24 hours");
		Check(desc.count(QStringLiteral("chance of precipitation")) == 24, "24 accessible samples");
		Check(desc.contains(QStringLiteral("Next 3 days")), "full layout includes the daily band");
		Check(desc.contains(QStringLiteral("0%")), "known zero precipitation is accessible");
		desc = Render(output, QStringLiteral("timeline-half") + theme, { 750, 982 }, frame, 14);
		Check(desc.contains(QStringLiteral("Next 12 hours")), "half-width layout selects 12 hours");
		Check(desc.contains(QStringLiteral("Next 3 days")), "half-width layout stacks the daily summaries");
		desc = Render(output, QStringLiteral("timeline-short") + theme, { 1100, 500 }, frame, 14);
		Check(desc.contains(QStringLiteral("Next 12 hours")), "short layout keeps twelve hours");
		Check(!desc.contains(QStringLiteral("Next 3 days")), "short layout drops the daily summaries");
		Check(desc.contains(QStringLiteral("Peak 80%")), "short layout keeps the precipitation peak");
	}
	auto day = Fixture(false, true, british);
	auto dry = day;
	for (auto &hour : dry.weather->hours) {
		hour.temperatureC = 5;
		hour.precipitation = 0;
	}
	auto desc = Render(output, QStringLiteral("timeline-dry-flat"), { 1512, 982 }, dry, 14);
	Check(desc.count(QStringLiteral("0%")) == 24, "confirmed dry hours keep their real zeroes");
	Check(!desc.contains(QStringLiteral("Peak ")), "dry forecast has no rain-lane summary");
	auto sparse = day;
	sparse.weather->hours.erase(sparse.weather->hours.begin() + 5);
	for (auto &hour : sparse.weather->hours) {
		hour.precipitation = std::nullopt;
	}
	sparse.weather->hours[7].temperatureC.reset();
	sparse.weather->days[1].highC.reset();
	desc = Render(output, QStringLiteral("timeline-sparse"), { 1512, 982 }, sparse, 14);
	Check(desc.contains(QStringLiteral("Next 23 hours")), "missing hours are not counted as samples");
	Check(desc.contains(QStringLiteral("Precipitation data unavailable")), "unknown rain is exposed as unavailable");
	Check(desc.contains(QStringLiteral("low \u221210° high –")), "a partial range retains its known endpoint");
	sparse.weather->hours[2].precipitation = 90;
	Render(output, QStringLiteral("timeline-mixed-rain"), { 750, 982 }, sparse, 14);
	auto twelveHour = Fixture(true, false, american);
	twelveHour.flat = true;
	twelveHour.fahrenheit = true;
	const auto zone = QTimeZone(QByteArray("Europe/Berlin"));
	const auto beforeFallback = QDateTime(QDate(2026, 10, 24), QTime(21, 30), QTimeZone::UTC);
	twelveHour.local = beforeFallback;
	twelveHour.weather->zone = zone;
	auto i = 0;
	for (auto &hour : twelveHour.weather->hours) {
		hour.time = beforeFallback.addSecs(1800 + 3600 * i++).toTimeZone(zone);
	}
	desc = Render(output, QStringLiteral("timeline-dst-fahrenheit"), { 1512, 982 }, twelveHour, 14);
	Check(desc.contains(QStringLiteral("AM")) && desc.contains(QStringLiteral("PM")), "12-hour forecast labels");
	const auto repeatedTime = american.toString(QTime(2, 0), QLocale::ShortFormat);
	Check(desc.contains(repeatedTime + QStringLiteral(" UTC+02:00"))
		&& desc.contains(repeatedTime + QStringLiteral(" UTC+01:00")),
		"both repeated hours have distinct offsets");
	Check(desc.contains(QStringLiteral("UTC+02:00")), "different forecast offset is shown");
	Check(desc.contains(QStringLiteral("46°F")), "current conditions use the chosen units");

	auto rtl = Fixture(true, false, QLocale(QLocale::Persian, QLocale::Iran));
	rtl.clocks.front().label = QStringLiteral("یک ساعت با نام پیکربندی‌شدهٔ طولانی");
	Render(output, QStringLiteral("timeline-rtl"), { 750, 982 }, rtl, 17);
	desc = Render(output, QStringLiteral("timeline-small"), { 360, 360 }, day, 14);
	Check(!desc.contains(QStringLiteral("Next 12 hours")) && desc.contains(QStringLiteral("Tehran")),
		"compact fallback retains the first world clock and current weather");
	desc = Render(output, QStringLiteral("timeline-large-type"), { 800, 500 }, day, 21);
	Check(desc.contains(QStringLiteral("Local weather")), "large type retains current conditions");
	auto current = day;
	current.weather->hours.clear();
	desc = Render(output, QStringLiteral("timeline-daily-only"), { 1270, 810 }, current, 14);
	Check(desc.contains(QStringLiteral("Next 3 days")) && !desc.contains(QStringLiteral(" hours")),
		"daily section stands alone");
	current.weather->days.clear();
	current.update = QStringLiteral("Offline · last updated 09:30 · Open-Meteo");
	desc = Render(output, QStringLiteral("timeline-current-only"), { 1270, 810 }, current, 14);
	Check(desc.contains(QStringLiteral("Offline")) && !desc.contains(QStringLiteral("Next ")),
		"current-only layout keeps its status");
	current.weather.reset();
	current.update.clear();
	Render(output, QStringLiteral("timeline-clocks-only"), { 1270, 810 }, current, 14);

	auto resized = Canvas();
	resized.resize(1512, 982);
	day.controlsTop = 900;
	resized.setFrame(day);
	resized.show();
	QApplication::processEvents();
	Check(resized.accessibleDescription().contains(QStringLiteral("Next 24 hours")), "resize fixture starts full");
	resized.resize(750, 982);
	QApplication::processEvents();
	Check(resized.accessibleDescription().contains(QStringLiteral("Next 12 hours")), "resize refreshes the accessible selection");
	std::cout << Checks << " checks, 0 failures\n";
}

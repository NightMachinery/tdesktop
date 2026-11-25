#include "purple/purple_privacy_canvas.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
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
	for (auto i = 0; i != 6; ++i) {
		const auto code = day ? (i < 2 ? 61 : i == 3 ? 2 : 3) : (i < 3 ? 0 : 3);
		weather.hours.push_back({ first.addSecs(i * 3600), day ? 14 + i / 2 : 8 - (i + 1) / 2,
			i == 0 ? 0 : day ? 85 - i * 9 : 4 + 2 * i, DescribeWmoCode(code), code });
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
	};
}

void Render(const QDir &output, const QString &name, QSize size, Frame frame, int pixels) {
	auto canvas = Canvas();
	auto font = QFont(QStringLiteral("Helvetica Neue"));
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
	if (name == QStringLiteral("privacy-narrow")) {
		const auto pixels = shot.toImage();
		auto moonPixels = 0;
		for (auto y = 0; y < pixels.height() / 4; ++y) {
			for (auto x = 3 * pixels.width() / 4; x < pixels.width(); ++x) {
				moonPixels += pixels.pixelColor(x, y) == QColor(0xffdc9f);
			}
		}
		Check(moonPixels > 50, "narrow clear-night motif survives beside the clock text");
	}
	const auto desc = canvas.accessibleDescription();
	Check(desc.contains(frame.locale.toString(frame.local.time(), QLocale::ShortFormat)),
		"widget reports the locale-formatted time");
	for (const auto &clock : frame.clocks) {
		if (clock.dayDifference == 1) {
			Check(desc.contains(QStringLiteral("Tomorrow")), "future relative day");
		} else if (clock.dayDifference == -1) {
			Check(desc.contains(QStringLiteral("Yesterday")), "previous relative day");
		}
	}
	if (frame.weather) {
		Check(desc.contains(QStringLiteral("0%")), "real zero precipitation is present in widget data");
		Check(desc.contains(QStringLiteral("Open-Meteo")), "weather attribution is visible to assistive technology");
	}
}

} // namespace

int main(int argc, char **argv) {
	const auto app = QApplication(argc, argv);
	Check(argc == 2, "pass a render directory");
	const auto output = QDir(QString::fromLocal8Bit(argv[1]));
	Check(output.mkpath(QStringLiteral(".")), "create the offscreen output directory");
	const auto british = QLocale(QLocale::English, QLocale::UnitedKingdom);
	const auto american = QLocale(QLocale::English, QLocale::UnitedStates);
	Check(british.toString(QTime(22, 15), QLocale::ShortFormat) == QStringLiteral("22:15"),
		"24-hour sample locale");
	Check(american.toString(QTime(22, 15), QLocale::ShortFormat).endsWith(QStringLiteral("PM")),
		"12-hour sample locale");
	auto night = Fixture(true, false, british);
	auto day = Fixture(false, true, british);
	Render(output, QStringLiteral("privacy-night-clear"), { 1280, 806 }, night, 14);
	Render(output, QStringLiteral("privacy-day-rain"), { 1280, 806 }, day, 14);
	Render(output, QStringLiteral("privacy-narrow"), { 430, 820 }, night, 14);
	Render(output, QStringLiteral("privacy-compact"), { 360, 360 }, night, 14);
	auto offline = day;
	offline.update = QStringLiteral("Offline · last updated 09:30 AM · Open-Meteo");
	Render(output, QStringLiteral("privacy-compact-offline"), { 360, 360 }, offline, 14);
	Render(output, QStringLiteral("privacy-rain-800"), { 800, 500 }, day, 14);
	Render(output, QStringLiteral("privacy-rain-large-text"), { 800, 500 }, day, 17);
	night.weather.reset();
	night.update.clear();
	Render(output, QStringLiteral("privacy-clocks-only"), { 1280, 806 }, night, 14);
	auto twelveHour = Fixture(true, false, american);
	twelveHour.flat = true;
	twelveHour.fahrenheit = true;
	Render(output, QStringLiteral("privacy-12-hour"), { 1280, 806 }, twelveHour, 14);
	auto yesterday = Fixture(false, true, british);
	yesterday.local = yesterday.local.addSecs(-8 * 3600);
	yesterday.clocks = Purple::PrivacyClocks::At(yesterday.local, yesterday.local.timeZone(), {
		{ QStringLiteral("America/New_York"), QStringLiteral("New York") },
	});
	Check(yesterday.clocks.front().dayDifference == -1, "relative day fixture crosses local midnight");
	yesterday.weather.reset();
	yesterday.update.clear();
	Render(output, QStringLiteral("privacy-yesterday"), { 800, 500 }, yesterday, 14);
	std::cout << Checks << " checks, 0 failures\n";
}

#pragma once

#include "purple/purple_privacy_clocks.h"
#include "purple/purple_privacy_weather.h"

#include <QtCore/QLocale>
#include <QtWidgets/QWidget>

#include <functional>

namespace Purple::PrivacyVisual {

#ifdef Q_OS_MAC
[[nodiscard]] bool PrefersFlatColors();
#endif // Q_OS_MAC

struct WeatherLabels {
	QString local;
	QString chance;
	QString unavailable;
	std::function<QString(int)> hours;
	std::function<QString(int)> days;
	std::function<QString(const QString&, const QString&)> peak;
};

struct Frame {
	QDateTime local;
	QLocale locale;
	std::vector<PrivacyClocks::Clock> clocks;
	std::optional<PrivacyWeather::WeatherSnapshot> weather;
	QString update;
	QString hint;
	int controlsTop = 0;
	int motifCode = 0;
	bool dark = false;
	bool day = false;
	bool flat = false;
	bool fahrenheit = false;
	WeatherLabels labels;
};

class Canvas final : public QWidget {
public:
	explicit Canvas(QWidget *parent = nullptr);
	void setFrame(Frame frame);

protected:
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void changeEvent(QEvent *event) override;

private:
	void refreshDescription();

	Frame _frame;
};

} // namespace Purple::PrivacyVisual

#pragma once

#include "purple/purple_privacy_clocks.h"
#include "purple/purple_privacy_weather.h"

#include <QtCore/QLocale>
#include <QtWidgets/QWidget>

namespace Purple::PrivacyVisual {

#ifdef Q_OS_MAC
[[nodiscard]] bool PrefersFlatColors();
#endif // Q_OS_MAC

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
};

class Canvas final : public QWidget {
public:
	explicit Canvas(QWidget *parent = nullptr);
	void setFrame(Frame frame);

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	Frame _frame;
};

} // namespace Purple::PrivacyVisual

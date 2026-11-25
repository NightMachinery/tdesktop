#include "ui/integration.h"
#include "ui/style/style_core.h"
#include "ui/widgets/continuous_sliders.h"
#include "styles/style_widgets.h"

#include <QtTest/QTest>
#include <QtWidgets/QApplication>

#include <cmath>
#include <cstdio>

namespace crl {
rpl::producer<> on_main_update_requests() {
	return rpl::never<>();
}
} // namespace crl

namespace {

class Integration final : public Ui::Integration {
public:
	void postponeCall(FnMut<void()> &&callable) override { callable(); }
	void registerLeaveSubscription(not_null<QWidget*>) override {}
	void unregisterLeaveSubscription(not_null<QWidget*>) override {}
	QString emojiCacheFolder() override { return {}; }
	QString fontsCacheFolder() override { return {}; }
	QString openglCheckFilePath() override { return {}; }
	int touchCounterIncrement() override { return 0; }
	int touchCounterNow() override { return 0; }
};

bool CheckProgress(bool rtl) {
	using Direction = Ui::ContinuousSlider::Direction;
	style::SetRightToLeft(rtl);
	auto slider = Ui::MediaSliderWheelless(nullptr, st::defaultContinuousSlider);
	slider.setDirection(rtl ? Direction::HorizontalReversed : Direction::Horizontal);
	slider.resize(240, 24);
	const auto played = QColor(27, 114, 182);
	const auto unplayed = QColor(127, 127, 127);
	slider.setColorOverrides({ .activeFg = played, .inactiveFg = unplayed });
	slider.setValue(.4);
	slider.show();
	qApp->processEvents();
	const auto rail = slider.grab().toImage();
	const auto pixel = [&](int x) {
		return rail.pixelColor(qRound(x * rail.devicePixelRatio()), rail.height() / 2);
	};
	const auto left = slider.width() / 5;
	const auto right = slider.width() - left;
	if (pixel(left) != (rtl ? unplayed : played)
			|| pixel(right) != (rtl ? played : unplayed)) {
		std::fprintf(stderr, "%s progress pixels did not match the rail direction.\n",
			rtl ? "RTL" : "LTR");
		return false;
	}
	auto result = -1.;
	slider.setChangeFinishedCallback([&](double value) { result = value; });
	QTest::mouseClick(&slider, Qt::LeftButton, Qt::NoModifier, QPoint(left, slider.height() / 2));
	const auto leftValue = result;
	QTest::mouseClick(&slider, Qt::LeftButton, Qt::NoModifier, QPoint(right, slider.height() / 2));
	if (std::abs(leftValue - (rtl ? .8 : .2)) > .01
			|| std::abs(result - (rtl ? .2 : .8)) > .01) {
		std::fprintf(stderr, "%s rail sought to %.3f and %.3f.\n", rtl ? "RTL" : "LTR", leftValue, result);
		return false;
	}
	std::printf("%s rail pixels and seek points passed.\n", rtl ? "RTL" : "LTR");
	return true;
}

} // namespace

int main(int argc, char **argv) {
	auto app = QApplication(argc, argv);
	auto integration = Integration();
	Ui::Integration::Set(&integration);
	auto animations = Ui::Animations::Manager();
	anim::SetDisabled(true);
	style::StartManager(100);
	return (CheckProgress(false) && CheckProgress(true)) ? 0 : 1;
}

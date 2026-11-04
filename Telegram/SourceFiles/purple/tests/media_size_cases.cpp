#include "purple/purple_media_rules.h"

#include <iostream>

int main() {
	auto checks = 0;
	auto failures = 0;
	const auto expect = [&](bool value, const char *name) {
		++checks;
		if (!value) { ++failures; std::cerr << name << '\n'; }
	};
	using namespace Purple::MediaRules;
	constexpr auto stock = 240;
	constexpr auto bound = 430;

	expect(VideoMessageScaleFrom(std::nullopt) == 200, "a missing preference reads 200%");
	expect(VideoMessageScaleFrom(100) == 100, "100% is kept");
	expect(VideoMessageScaleFrom(150) == 150, "150% is kept");
	expect(VideoMessageScaleFrom(250) == 250, "250% is kept");
	expect(VideoMessageScaleFrom(175) == 200, "a size not offered reads the default");
	expect(VideoMessageScaleFrom(-200) == 200, "a negative size reads the default");
	expect(VideoMessageScaleFrom(1000) == 200, "a huge size reads the default");

	expect(VideoMessageScaleFrom(0) == 0, "zero is the actual size");
	expect(VideoMessageScaleFromText("") == 200, "an empty preference reads 200%");
	expect(VideoMessageScaleFromText("actual") == 0, "the actual size is read back");
	expect(VideoMessageScaleFromText("150") == 150, "a stored percent is read back");
	expect(VideoMessageScaleFromText("0") == 200, "a stored zero reads the default");
	expect(VideoMessageScaleFromText("175") == 200, "a stored size not offered reads the default");
	expect(VideoMessageScaleFromText("150x") == 200, "a malformed size reads the default");
	expect(VideoMessageScaleFromText("-150") == 200, "a negative stored size reads the default");

	expect(VideoMessageActualSide(400, 1.) == 400, "actual size on a 1x screen");
	expect(VideoMessageActualSide(400, 2.) == 200, "actual size on a 2x screen");
	expect(VideoMessageActualSide(384, 2.) == 192, "a phone round on a 2x screen");
	expect(VideoMessageActualSide(0, 2.) == 200, "an unknown source uses the desktop recording side");
	expect(VideoMessageActualSide(400, 0.) == 400, "a ratio under 1 counts as 1");
	expect(VideoMessageActualPercent(400, 2., stock) == 83, "the actual size label on a 2x screen");
	expect(VideoMessageActualPercent(400, 1., stock) == 167, "the actual size label on a 1x screen");
	expect(VideoMessageActualPercent(400, 2., 360) == 56, "the label follows the interface scale");

	expect(VideoMessageSide(100, stock) == 240, "100% is the stock side");
	expect(VideoMessageSide(150, stock) == 360, "150% side");
	expect(VideoMessageSide(200, stock) == 480, "200% side");
	expect(VideoMessageSide(250, stock) == 600, "250% side");
	expect(VideoMessageSide(200, 0) == 1, "the side never reaches zero");

	expect(VideoMessageThumbSide(480, bound, bound) == 480, "the optimal pass is not capped at the bubble width");
	expect(VideoMessageThumbSide(360, bound, bound) == 360, "a side under the bound stays");
	expect(VideoMessageThumbSide(480, 600, bound) == 480, "a wide column keeps the side");
	expect(VideoMessageThumbSide(480, 300, bound) == 300, "a narrow column clamps to its width");
	expect(VideoMessageThumbSide(480, 480, bound) == 480, "an exact fit keeps the side");
	expect(VideoMessageThumbSide(480, 0, bound) == 1, "no room still gives one pixel");

	expect(VideoMessageBubbleLimit(480, bound) == 480, "a round wider than the bubble limit raises it");
	expect(VideoMessageBubbleLimit(560, bound) == 560, "the reply room beside the round is kept");
	expect(VideoMessageBubbleLimit(430, bound) == 0, "a round at the limit leaves upstream's");
	expect(VideoMessageBubbleLimit(360, bound) == 0, "a smaller round leaves upstream's");

	constexpr auto floatStock = 152;
	constexpr auto margins = 24;
	expect(FloatPlayerSide(floatStock, margins, 480, 1600) == 504, "the float keeps the in-chat side");
	expect(FloatPlayerSide(floatStock, margins, 480, 800) == 400, "a small window caps the float at half its shorter side");
	expect(FloatPlayerSide(floatStock, margins, 480, 200) == floatStock, "a tiny window never shrinks the float below stock");
	expect(FloatPlayerSide(floatStock, margins, 240, 1600) == 264, "100% keeps the stock in-chat side");
	expect(FloatPlayerSide(floatStock, margins, 100, 1600) == floatStock, "a small actual size keeps the stock float");
	expect(FloatPlayerSide(floatStock, margins, 480, 0) == 504, "an unknown window does not clamp");

	std::cout << (checks - failures) << '/' << checks << " media size checks passed\n";
	return failures ? 1 : 0;
}

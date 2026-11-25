#pragma once

#include <QtGui/QColor>

#include <algorithm>
#include <cmath>

namespace Purple::ModalUi {

[[nodiscard]] inline QColor BlendColor(QColor from, QColor to, double ratio) {
	const auto blend = [&](int first, int second) {
		return int(std::lround(first + (second - first) * ratio));
	};
	return QColor(
		blend(from.red(), to.red()),
		blend(from.green(), to.green()),
		blend(from.blue(), to.blue()));
}

[[nodiscard]] inline double TextContrast(QColor ink, QColor surface) {
	const auto luminance = [](QColor color) {
		const auto linear = [](int channel) {
			const auto value = channel / 255.;
			return (value <= 0.04045)
				? value / 12.92
				: std::pow((value + 0.055) / 1.055, 2.4);
		};
		return 0.2126 * linear(color.red())
			+ 0.7152 * linear(color.green())
			+ 0.0722 * linear(color.blue());
	};
	const auto inkLuminance = luminance(ink);
	const auto surfaceLuminance = luminance(surface);
	const auto [low, high] = std::minmax(inkLuminance, surfaceLuminance);
	return (high + 0.05) / (low + 0.05);
}

[[nodiscard]] inline QColor ReadableText(QColor ink, QColor surface) {
	constexpr auto kMinimumRatio = 4.5;
	if (TextContrast(ink, surface) >= kMinimumRatio) {
		return ink;
	}
	const auto black = QColor(Qt::black);
	const auto white = QColor(Qt::white);
	const auto end = (TextContrast(black, surface) > TextContrast(white, surface))
		? black : white;
	auto low = 0;
	auto high = 256;
	while (high - low > 1) {
		const auto middle = (low + high) / 2;
		if (TextContrast(BlendColor(ink, end, middle / 256.), surface) >= kMinimumRatio) {
			high = middle;
		} else {
			low = middle;
		}
	}
	return BlendColor(ink, end, high / 256.);
}

} // namespace Purple::ModalUi

#include "purple/purple_keys_colors.h"

#include <array>
#include <cstdlib>
#include <iostream>

using namespace Purple::ModalUi;

int main() {
	const auto palettes = std::array{
		std::array{ QColor(0xFFFFFF), QColor(0x999999), QColor(0x168ACD) },
		std::array{ QColor(0x16202A), QColor(0x75899C), QColor(0x69B7F0) },
	};
	const auto teal = QColor(0x0F766E);
	const auto green = QColor(0x2F7D44);
	const auto amber = QColor(0xB45309);
	if (TextContrast(QColor(Qt::black), QColor(Qt::white)) != 21.) {
		return EXIT_FAILURE;
	}
	auto checks = 1;
	for (const auto &[background, muted, active] : palettes) {
		const auto cases = std::array{
			std::pair{ teal, background },
			std::pair{ teal, BlendColor(background, teal, 28. / 255.) },
			std::pair{ green, BlendColor(background, green, 26. / 255.) },
			std::pair{ amber, BlendColor(background, amber, 26. / 255.) },
			std::pair{ muted, background },
			std::pair{ active, background },
		};
		for (const auto &[ink, surface] : cases) {
			const auto adjusted = ReadableText(ink, surface);
			const auto ratio = TextContrast(adjusted, surface);
			std::cout << surface.name().toStdString() << ' '
				<< adjusted.name().toStdString() << ' ' << ratio << ":1\n";
			if (ratio < 4.5 || (TextContrast(ink, surface) >= 4.5 && ink != adjusted)) {
				return EXIT_FAILURE;
			}
			++checks;
		}
	}
	for (auto gray = 0; gray != 256; ++gray) {
		const auto surface = QColor(gray, gray, gray);
		if (TextContrast(ReadableText(teal, surface), surface) < 4.5) {
			return EXIT_FAILURE;
		}
		++checks;
	}
	std::cout << checks << " checks, 0 failures\n";
}

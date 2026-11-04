#pragma once

#include <algorithm>

namespace Purple {

struct RoundRecordingGeometry {
	int corner = 3;
	int x = 0;
	int y = 0;

	void clamp(int width, int height, int side) {
		x = std::clamp(x, 0, std::max(0, width - side));
		y = std::clamp(y, 0, std::max(0, height - side));
	}
	void snap(int width, int height, int side, int inset) {
		corner = ((x + side / 2 >= width / 2) ? 1 : 0)
			| ((y + side / 2 >= height / 2) ? 2 : 0);
		place(width, height, side, inset);
	}
	void place(int width, int height, int side, int inset) {
		x = (corner & 1) ? width - side - inset : inset;
		y = (corner & 2) ? height - side - inset : inset;
		clamp(width, height, side);
	}
};

} // namespace Purple

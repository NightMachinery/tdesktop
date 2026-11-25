#include "purple/purple_privacy_canvas.h"

#include <AppKit/AppKit.h>

namespace Purple::PrivacyVisual {

bool PrefersFlatColors() {
	const auto workspace = [NSWorkspace sharedWorkspace];
	return [workspace accessibilityDisplayShouldIncreaseContrast]
		|| [workspace accessibilityDisplayShouldReduceMotion];
}

} // namespace Purple::PrivacyVisual

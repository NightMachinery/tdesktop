#include "purple/purple_ui_witness.h"

#include <AppKit/AppKit.h>
#include <QWidget>
#include <cmath>

namespace Purple::UiWitness {

std::optional<NativeWindow> ResolveNativeWindow(QWidget *widget) {
	if (!widget || !widget->isVisible() || !widget->window()->isVisible()) {
		return std::nullopt;
	}
	const auto handle = widget->effectiveWinId();
	if (!handle) {
		return std::nullopt;
	}
	const auto view = (__bridge NSView*)reinterpret_cast<void*>(handle);
	const auto window = view.window;
	const auto screens = NSScreen.screens;
	if (!window || screens.count != 1 || !window.isVisible
		|| window.windowNumber <= 0 || window.windowNumber > INT_MAX
		|| window.backingScaleFactor != 2.
		|| widget->devicePixelRatioF() != 2.) {
		return std::nullopt;
	}
	const auto screen = screens.firstObject;
	const auto f = window.frame;
	const auto x = f.origin.x;
	const auto y = NSMaxY(screen.frame) - NSMaxY(f);
	const auto w = f.size.width;
	const auto h = f.size.height;
	if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w)
		|| !std::isfinite(h) || std::abs(x) > 8192 || std::abs(y) > 8192
		|| w <= 0 || h <= 0 || w > 1920 || h > 1080
		|| x != std::floor(x) || y != std::floor(y)
		|| w != std::floor(w) || h != std::floor(h)) {
		return std::nullopt;
	}
	const auto frame = QRect(int(x), int(y), int(w), int(h));
	if (widget->window()->frameGeometry() != frame) {
		return std::nullopt;
	}
	const auto origin = widget->mapToGlobal(QPoint());
	if (!frame.contains(origin)) {
		return std::nullopt;
	}
	return NativeWindow{
		.number = quint32(window.windowNumber),
		.frame = frame,
		.origin = origin,
		.scale = 2048,
	};
}

}

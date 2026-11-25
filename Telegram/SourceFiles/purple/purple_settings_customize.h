#pragma once

#include "base/basic_types.h"

#include <optional>

class QEvent;
class QObject;
class QWidget;

namespace Window {
class Controller;
} // namespace Window

namespace Purple {

[[nodiscard]] bool CanCustomizeSettings(Window::Controller *window);
void ShowCustomizeSettings(
	not_null<Window::Controller*> window,
	QWidget *returnFocus = nullptr);
[[nodiscard]] std::optional<bool> FilterCustomizeSettingsInput(QObject *receiver, QEvent *event);

} // namespace Purple

#pragma once

#include "base/basic_types.h"

namespace Window {
class Controller;
} // namespace Window

namespace Purple::Diagnostics {

[[nodiscard]] bool CanManage(Window::Controller *window);
void ShowExport(not_null<Window::Controller*> window);
void ShowClear(not_null<Window::Controller*> window);

} // namespace Purple::Diagnostics

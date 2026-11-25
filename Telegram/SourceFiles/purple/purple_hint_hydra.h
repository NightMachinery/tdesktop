#pragma once

#include "purple/purple_hint_targets.h"

#include <memory>

namespace Ui {
class LayerWidget;
} // namespace Ui

namespace Purple {

[[nodiscard]] std::unique_ptr<Ui::LayerWidget> CreateHintHydra(
	WindowHintTarget target,
	WindowHintTarget::Menu menu,
	Fn<bool()> allowed,
	Fn<void()> stale);

} // namespace Purple

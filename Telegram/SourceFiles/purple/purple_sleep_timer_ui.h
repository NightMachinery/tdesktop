#pragma once

#include "data/data_audio_msg_id.h"
#include "ui/widgets/menu/menu_add_action_callback.h"

namespace Ui { class RpWidget; class Show; }
namespace ChatHelpers { class Show; }

namespace Purple {

void DecorateSleepTimer(
	not_null<Ui::RpWidget*> bar,
	not_null<QWidget*> menuParent,
	std::shared_ptr<ChatHelpers::Show> show,
	Fn<void()> relayout,
	Fn<void(bool)> menuOver,
	Fn<bool(QPoint)> otherDropdown);
[[nodiscard]] not_null<Ui::RpWidget*> SleepTimerControl(not_null<QWidget*> bar);
void UpdateSleepTimerVisibility(not_null<QWidget*> bar, bool secondaryVisible);
void PlaceSleepTimer(not_null<QWidget*> bar, int right, int top);
[[nodiscard]] int SleepTimerButtonWidth(not_null<const QWidget*> bar);
[[nodiscard]] bool OverSleepTimerDropdown(not_null<const QWidget*> bar, QPoint global);
void HideSleepTimerDropdown(not_null<QWidget*> bar, not_null<Ui::RpWidget*> except);
void UpdateSleepTimerDropdown(not_null<QWidget*> bar);
void SuspendSleepTimerDropdown(not_null<QWidget*> bar, bool hidden);
void AddSleepTimerViewerActions(
	const Ui::Menu::MenuCallback &add,
	std::shared_ptr<ChatHelpers::Show> show,
	const void *surface);

} // namespace Purple

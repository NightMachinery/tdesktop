#pragma once

class QRect;
class QWidget;

namespace Ui {
class ChatTheme;
class RpWidget;
} // namespace Ui

namespace Purple {

void WatchChatBackgroundVisibility(not_null<Ui::RpWidget*> widget);

[[nodiscard]] bool PaintHiddenChatBackground(
	not_null<QWidget*> widget,
	not_null<Ui::ChatTheme*> theme,
	const QRect &clip);

void ToggleChatBackgroundVisibility(not_null<QWidget*> window);

} // namespace Purple

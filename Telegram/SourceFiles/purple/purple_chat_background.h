#pragma once

class QRect;
class QWidget;

namespace Ui {
class ChatStyle;
class ChatTheme;
class RpWidget;
} // namespace Ui

namespace Purple {

void WatchChatBackgroundVisibility(not_null<Ui::RpWidget*> widget);

void WatchLiveChatBackgroundStyle(
	not_null<Ui::ChatStyle*> style,
	Fn<std::shared_ptr<Ui::ChatTheme>()> currentTheme,
	rpl::lifetime &lifetime);

void ApplyLiveChatBackgroundStyle(
	not_null<Ui::ChatStyle*> style,
	not_null<Ui::ChatTheme*> theme);

[[nodiscard]] bool PaintHiddenChatBackground(
	not_null<QWidget*> widget,
	not_null<Ui::ChatTheme*> theme,
	const QRect &clip);

void ToggleChatBackgroundVisibility(not_null<QWidget*> window);

} // namespace Purple

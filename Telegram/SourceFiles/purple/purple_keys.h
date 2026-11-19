#pragma once

class QEvent;
class QKeyEvent;
class QObject;
class QWidget;

namespace Ui {
class LayerWidget;
class RpWidget;
} // namespace Ui

namespace Settings::Builder {
class SectionBuilder;
} // namespace Settings::Builder

namespace Purple {

[[nodiscard]] bool HandleModalKeys(QObject *receiver, QEvent *event);
[[nodiscard]] bool ModalKeysOwnChatListFocus(QWidget *widget);
[[nodiscard]] bool CalendarDayKeysEnabled();
[[nodiscard]] int CalendarDayKeyOffset(const QKeyEvent &event);

void InstallMainMenuKeys(
	not_null<Ui::LayerWidget*> menu,
	not_null<Ui::RpWidget*> contents);

void BuildModalKeysSetting(::Settings::Builder::SectionBuilder &builder);

} // namespace Purple

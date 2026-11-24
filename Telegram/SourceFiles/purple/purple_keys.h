#pragma once

#include <QtCore/QRect>

class MainWindow;
class QEvent;
class QKeyEvent;
class QObject;
class QWidget;

namespace Ui {
class AbstractButton;
class LayerWidget;
class RpWidget;
template <typename Widget>
class SlideWrap;
} // namespace Ui

namespace Settings::Builder {
class SectionBuilder;
} // namespace Settings::Builder

namespace Purple {

[[nodiscard]] bool HandleModalKeys(QObject *receiver, QEvent *event);
[[nodiscard]] bool ModalKeysOwnChatListFocus(QWidget *widget);
[[nodiscard]] bool CalendarDayKeysEnabled();
[[nodiscard]] int CalendarDayKeyOffset(const QKeyEvent &event);

void EnsureModalKeysAttached(not_null<MainWindow*> window);
[[nodiscard]] int ModalKeysStatusHeight(not_null<MainWindow*> window);
void UpdateModalKeysLayout(not_null<MainWindow*> window, QRect body);
void RaiseModalKeysChrome(not_null<MainWindow*> window);

void InstallMainMenuKeys(
	not_null<Ui::LayerWidget*> menu,
	not_null<Ui::RpWidget*> contents,
	not_null<Ui::AbstractButton*> accountsToggle,
	not_null<Ui::SlideWrap<Ui::RpWidget>*> accounts);

void BuildModalKeysSetting(::Settings::Builder::SectionBuilder &builder);

} // namespace Purple

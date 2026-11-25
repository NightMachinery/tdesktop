#pragma once

#include "purple/purple_notice_state.h"

#include <QtCore/QRect>
#include <QtCore/QString>

#include <memory>

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

[[nodiscard]] bool ModalKeysEnabled();
[[nodiscard]] bool ModalKeysInNormal(not_null<MainWindow*> window);
[[nodiscard]] rpl::producer<bool> ModalKeysEnabledChanges();
[[nodiscard]] bool MxShortcutInStockMode();
[[nodiscard]] rpl::producer<QString> MxShortcutInStockModeLabel();
[[nodiscard]] rpl::producer<bool> MxShortcutInStockModeValue();
void SetMxShortcutInStockMode(bool enabled);
[[nodiscard]] ModalNotices::DeliveryContext ModalDeliveryContext(MainWindow *window);
void SetModalStatusMessage(
	MainWindow *window,
	QString text,
	bool isError,
	int durationMs,
	bool persistent);
void RefreshModalNoticeIndicator(MainWindow *window);
void ShowModalNoticeLayer(
	not_null<MainWindow*> window,
	std::unique_ptr<Ui::LayerWidget> layer,
	bool isHistory);

} // namespace Purple

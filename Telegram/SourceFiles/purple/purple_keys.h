#pragma once

#include "purple/purple_notice_state.h"

#include <QtCore/QRect>
#include <QtCore/QString>

#include <memory>

class MainWindow;
class QEvent;
class QKeyEvent;
class QObject;
class QAbstractScrollArea;
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

namespace Window {
class SessionController;
} // namespace Window

namespace Purple {

[[nodiscard]] QString ZenChatListToggleLabel(
	not_null<Window::SessionController*> controller);
void ToggleZenChatList(not_null<Window::SessionController*> controller);

struct MusicShortcutHints;
[[nodiscard]] MusicShortcutHints MusicEffectiveShortcuts();

[[nodiscard]] bool HandleModalKeys(QObject *receiver, QEvent *event);
[[nodiscard]] bool HandleModalWindowHints(QWidget *receiver, QEvent *event);
[[nodiscard]] int ModalPhysicalKey(const QKeyEvent &event);
[[nodiscard]] bool AllowModalLayerKey(
	const QKeyEvent &event,
	not_null<QWidget*> layer,
	bool motion = false);
void CloseModalFilterLayer(not_null<Ui::LayerWidget*> layer);
[[nodiscard]] Qt::KeyboardModifiers ModalScrollModifiers(
	const QKeyEvent &event,
	not_null<QWidget*> layer);
void ScrollModalLayer(
	const QKeyEvent &event,
	not_null<QWidget*> layer,
	not_null<QAbstractScrollArea*> scroll,
	int delta);
#ifdef Q_OS_MAC
[[nodiscard]] bool SwallowModalNativeRepeat(QWidget *window, int nativeKey);
#endif // Q_OS_MAC
[[nodiscard]] bool ModalKeysOwnChatListFocus(QWidget *widget);
[[nodiscard]] bool CalendarDayKeysEnabled();
[[nodiscard]] int CalendarDayKeyOffset(const QKeyEvent &event);

void ModalFullscreenChanged(not_null<MainWindow*> window, bool fullscreen);
void EnsureModalKeysAttached(not_null<MainWindow*> window);
[[nodiscard]] int ModalKeysStatusHeight(not_null<MainWindow*> window);
void UpdateModalKeysLayout(not_null<MainWindow*> window, QRect body);
void RefreshModalKeysColumns(not_null<MainWindow*> window);
void RaiseModalKeysChrome(not_null<MainWindow*> window);

void InstallMainMenuKeys(
	not_null<Ui::LayerWidget*> menu,
	not_null<Ui::RpWidget*> contents,
	not_null<Ui::AbstractButton*> accountsToggle,
	not_null<Ui::SlideWrap<Ui::RpWidget>*> accounts);

void BuildModalKeysSetting(::Settings::Builder::SectionBuilder &builder);

[[nodiscard]] bool ModalKeysEnabled();
[[nodiscard]] bool ModalKeysInNormal(not_null<MainWindow*> window);
void AcceptMusicFilter(not_null<MainWindow*> window);
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

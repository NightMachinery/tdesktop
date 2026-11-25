#pragma once

#include "base/basic_types.h"
#include "ui/rp_widget.h"

#include <QtCore/QString>

class MainWindow;
namespace Window {
class MainWindow;
class SessionController;
} // namespace Window

namespace Settings::Builder {
class SectionBuilder;
} // namespace Settings::Builder

namespace Purple {
namespace PrivacyVisual {
class Canvas;
} // namespace PrivacyVisual

// Non-private display backdrop (clock, date, optional weather).
class PrivacyBackdropWidget final : public Ui::RpWidget {
public:
	explicit PrivacyBackdropWidget(not_null<QWidget*> parent);
	~PrivacyBackdropWidget() override;

	void setInteractiveHint(const QString &hint);
	void setControlsTop(int top);

protected:
	void resizeEvent(QResizeEvent *event) override;
	void showEvent(QShowEvent *event) override;

private:
	void refreshWeatherIfConfigured();
	void refreshCanvas();

	PrivacyVisual::Canvas *_canvas = nullptr;
	QString _hint;
	int _controlsTop = 0;
	rpl::lifetime _weatherLifetime;
};

[[nodiscard]] bool PrivacyScreenActiveAny();
[[nodiscard]] rpl::producer<bool> PrivacyScreenChanges();
[[nodiscard]] bool PrivacyScreenActive(const ::Window::MainWindow *window);
void TogglePrivacyScreen(not_null<::MainWindow*> window);
void DismissPrivacyScreen();
void SyncPrivacyScreenForWindow(not_null<::MainWindow*> window);
void UpdatePrivacyScreenLayout(not_null<::MainWindow*> window, QRect body);
void RaisePrivacyScreen(not_null<::MainWindow*> window);

void ShowPrivacyWeatherSettings(not_null<Window::SessionController*> controller);
void BuildPrivacyScreenSetting(::Settings::Builder::SectionBuilder &builder);

} // namespace Purple

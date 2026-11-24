#pragma once

#include "base/basic_types.h"
#include "ui/rp_widget.h"

#include <QtCore/QString>

class MainWindow;
namespace Window {
class MainWindow;
} // namespace Window

namespace Settings::Builder {
class SectionBuilder;
} // namespace Settings::Builder

namespace Purple {

[[nodiscard]] QString PrivacyWeatherLocation();
void SetPrivacyWeatherLocation(const QString &location);

// Non-private display backdrop (clock, date, optional weather).
class PrivacyBackdropWidget final : public Ui::RpWidget {
public:
	explicit PrivacyBackdropWidget(not_null<QWidget*> parent);
	~PrivacyBackdropWidget() override;

	void setInteractiveHint(const QString &hint);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	void refreshWeatherIfConfigured();

	QString _hint;
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

void BuildPrivacyScreenSetting(::Settings::Builder::SectionBuilder &builder);

} // namespace Purple

#pragma once

#include <QtCore/QString>
#include <rpl/producer.h>

namespace Settings::Builder {
class SectionBuilder;
}

namespace Purple {

void StartControlServer();
void BuildControlSettings(::Settings::Builder::SectionBuilder &builder);
[[nodiscard]] QString CurrentControlSocketPath();
[[nodiscard]] rpl::producer<QString> ControlSocketPathValue();
[[nodiscard]] rpl::producer<QString> ControlSocketStatusValue();
[[nodiscard]] rpl::producer<> ControlSettingsChanges();
[[nodiscard]] bool ControlAutomationEnabled();
void DisableControlAutomation();
[[nodiscard]] quint64 BeginControlConsent();
[[nodiscard]] bool AdvanceControlConsent(quint64 token);
[[nodiscard]] bool ConfirmControlConsent(quint64 token);
void CancelControlConsent(quint64 token);
[[nodiscard]] bool ControlGroupEnabled(int index);
void SetControlGroupEnabled(int index, bool enabled);
[[nodiscard]] QString ConfiguredControlSocketPath();
[[nodiscard]] bool SetControlSocketPath(const QString &path);

}

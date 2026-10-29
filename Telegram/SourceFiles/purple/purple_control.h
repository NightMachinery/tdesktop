#pragma once

#include <QtCore/QString>
#include <rpl/producer.h>

namespace Settings::Builder {
class SectionBuilder;
}

namespace Purple {

enum class ControlLockUnit;
struct ControlPolicy;

void StartControlServer();
void StartControlPlayerEvents();
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
[[nodiscard]] ControlPolicy CurrentControlPolicy();
[[nodiscard]] bool ControlAllowLocked(ControlLockUnit unit);
void SetControlAllowLocked(ControlLockUnit unit, bool allowed);
[[nodiscard]] bool ControlEmojiAllowedWhileLocked();
[[nodiscard]] bool ControlGroupEnabled(int index);
void SetControlGroupEnabled(int index, bool enabled);
[[nodiscard]] QString ConfiguredControlSocketPath();
[[nodiscard]] bool SetControlSocketPath(const QString &path);

}

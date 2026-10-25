/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_account_config.h"
#include "purple/purple_wait_gate.h"

#include <rpl/producer.h>

namespace Main { class Session; }

namespace Ui {
class RpWidget;
} // namespace Ui

namespace Purple {

struct PeekChange;
struct AppWideSettingsEditorState;

[[nodiscard]] inline auto PreparePeekDelayChange(
	const std::optional<AccountSettingsWriteTarget> &opening,
	const std::function<bool()> &originCurrent,
	const std::function<bool(const AccountSettingsWriteTarget&)> &validate)
-> std::optional<AccountSettingsWriteTarget> {
	const auto captured = opening;
	if (!captured || captured->destination.role != SettingsOpenRole::AppWide
		|| !originCurrent || !originCurrent() || !validate || !validate(*captured)
		|| !originCurrent()) {
		return std::nullopt;
	}
	return captured;
}

struct WaitRequest {
	WaitAction action = WaitAction::StartPeek;
	Main::Session *session = nullptr;
	int seconds = 0;
	QString preset;
	std::shared_ptr<AppWideSettingsEditorState> editor;
	bool same = false;
	bool fromBypass = false;
	bool toBypass = false;
	std::function<QString()> validate;
	std::function<QString(const std::function<bool()>&)> apply;
	std::function<void()> finished;
};

WaitResult RequestWait(WaitRequest request);

void RegisterPeekWindow(not_null<Ui::RpWidget*> widget);
[[nodiscard]] bool PeekStartPending();
[[nodiscard]] bool PeekCountdownPending();
[[nodiscard]] rpl::producer<> PeekCountdownChanges();
void CancelPendingPeek();
void EndPeekOnApplicationLock();
[[nodiscard]] bool PeekEndsOnFocusLoss();
[[nodiscard]] rpl::producer<bool> PeekFocusLossValue();
void RequestPeekFocusLossChange(bool enabled, not_null<Main::Session*> session,
	std::function<bool()> editorCurrent = {});
void RequestPeekDelayChange(
	int seconds,
	const std::shared_ptr<AppWideSettingsEditorState> &editor,
	not_null<Main::Session*> session);
void CloseHiddenPeekChats();

namespace details {
void RefreshWaitSettings();
PeekChange ActivatePeekFor(int seconds);
PeekChange ActivatePeekExtension(int seconds);
PeekChange DeactivatePeek();
} // namespace details

} // namespace Purple

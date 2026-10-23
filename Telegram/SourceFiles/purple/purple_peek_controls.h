/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_account_config.h"

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

void RegisterPeekWindow(not_null<Ui::RpWidget*> widget);
[[nodiscard]] bool PeekStartPending();
[[nodiscard]] bool PeekCountdownPending();
[[nodiscard]] rpl::producer<> PeekCountdownChanges();
void CancelPendingPeek();
void RequestPeekDelayChange(
	int seconds,
	const std::shared_ptr<AppWideSettingsEditorState> &editor,
	not_null<Main::Session*> session);
void CloseHiddenPeekChats();

namespace details {
PeekChange ActivatePeekFor(int seconds);
} // namespace details

} // namespace Purple

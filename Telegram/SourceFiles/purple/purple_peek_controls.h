/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <rpl/producer.h>

namespace Ui {
class RpWidget;
} // namespace Ui

namespace Purple {

struct PeekChange;

void RegisterPeekWindow(not_null<Ui::RpWidget*> widget);
[[nodiscard]] bool PeekStartPending();
[[nodiscard]] bool PeekCountdownPending();
[[nodiscard]] rpl::producer<> PeekCountdownChanges();
void CancelPendingPeek();
void RequestPeekDelayChange(int seconds);
void CloseHiddenPeekChats();

namespace details {
PeekChange ActivatePeekFor(int seconds);
} // namespace details

} // namespace Purple

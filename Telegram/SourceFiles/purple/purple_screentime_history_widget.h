/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "purple/purple_screentime_cover.h"
#include "purple/purple_screentime_recorder.h"

#include <memory>

namespace Purple {

[[nodiscard]] std::unique_ptr<ScreenTimeCover> MakeHistoryScreenTimeCover(
		not_null<Ui::RpWidget*> parent);
void SetHistoryWidgetScreenTimeCoverPeer(
		not_null<ScreenTimeCover*> cover,
		PeerData *peer);
void SetHistoryWidgetScreenTimeCoverGeometry(
		not_null<ScreenTimeCover*> cover,
		int top,
		int width,
		int height);

} // namespace Purple

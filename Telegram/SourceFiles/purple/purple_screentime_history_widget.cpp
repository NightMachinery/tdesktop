/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_screentime_history_widget.h"

#include <algorithm>

#include <QtCore/QRect>

namespace Purple {

std::unique_ptr<ScreenTimeCover> MakeHistoryScreenTimeCover(
		not_null<Ui::RpWidget*> parent) {
	return std::make_unique<ScreenTimeCover>(parent);
}

void SetHistoryWidgetScreenTimeCoverPeer(
		not_null<ScreenTimeCover*> cover,
		PeerData *peer) {
	cover->setPeer(peer);
}

void SetHistoryWidgetScreenTimeCoverGeometry(
		not_null<ScreenTimeCover*> cover,
		int top,
		int width,
		int height) {
	cover->setGeometry(QRect(0, top, width, std::max(height - top, 0)));
}

} // namespace Purple

/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "base/platform/base_platform_system_media_controls.h"
#include "data/data_audio_msg_id.h"

#include <QtCore/QString>

class QWidget;

namespace Ui {
class RpWidget;
} // namespace Ui

namespace Purple {

void DecoratePlayerSeek(
	not_null<Ui::RpWidget*> controls,
	Fn<AudioMsgId::Type()> type);
[[nodiscard]] int PlacePlayerSeek(not_null<Ui::RpWidget*> controls, int right);
[[nodiscard]] int PlayerSeekWidth(not_null<const QWidget*> player);

[[nodiscard]] QString SeekShortcutKeys(int direction);

[[nodiscard]] bool HandleSystemMediaCommand(
	base::Platform::SystemMediaControls::Command command);

} // namespace Purple

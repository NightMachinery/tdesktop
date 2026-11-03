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

namespace Main {
class Session;
} // namespace Main

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

void MirrorRecordingWrite(
	const void *owner,
	int64 offset,
	const uint8_t *data,
	int size);
void NoteRecordingStop(
	not_null<Main::Session*> session,
	int type,
	bool listen,
	bool viewOnce);

} // namespace Purple

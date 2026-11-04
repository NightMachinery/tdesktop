/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/hooks/media_recording.h"

#include <array>

namespace Settings::Builder {
class SectionBuilder;
} // namespace Settings::Builder

namespace Media::Player {
class Float;
} // namespace Media::Player

namespace Ui {
class AbstractButton;
} // namespace Ui

namespace Purple {

[[nodiscard]] int SeekStepSeconds();
bool SeekBy(AudioMsgId::Type type, int direction);

void StartMediaSeek();

[[nodiscard]] auto AddFloatSeekButtons(
	not_null<Media::Player::Float*> floating)
-> std::array<not_null<Ui::AbstractButton*>, 2>;

void AddSeekStepSetting(::Settings::Builder::SectionBuilder &builder);

} // namespace Purple

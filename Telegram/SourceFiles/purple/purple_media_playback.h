/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/hooks/media_playback.h"

namespace Settings::Builder {
class SectionBuilder;
} // namespace Settings::Builder

namespace Purple {

[[nodiscard]] int VideoMessageScale();
void SetVideoMessageScale(int percent);
[[nodiscard]] int VideoMessageSide(const DocumentData *document);

void BuildMediaPlaybackSettings(::Settings::Builder::SectionBuilder &builder);

} // namespace Purple

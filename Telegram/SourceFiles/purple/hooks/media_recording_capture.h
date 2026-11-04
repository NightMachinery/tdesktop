/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <cstdint>

struct AVFormatContext;
struct AVPacket;

namespace Main {
class Session;
} // namespace Main

namespace Purple {

void MirrorRecordingWrite(
	const void *owner,
	int64 offset,
	const uint8_t *data,
	int size);
void NoteRecordingStop(
	not_null<Main::Session*> session,
	int type,
	bool viewOnce);
void OnRoundEncodingStarted(const void *owner, AVFormatContext *format);
void OnRoundPacket(const void *owner, const AVPacket *packet);

} // namespace Purple

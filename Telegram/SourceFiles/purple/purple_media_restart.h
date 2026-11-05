/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "data/data_audio_msg_id.h"
#include <rpl/producer.h>

class QWidget;

namespace Purple {

void QueueHistoryMediaRestart(
	not_null<QWidget*> context,
	rpl::producer<> playerLifetime,
	AudioMsgId identity,
	Fn<AudioMsgId()> current,
	Fn<void()> restart);

} // namespace Purple

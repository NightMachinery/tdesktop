/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_media_rules.h"

namespace Purple::MediaRules {

bool RestartsOnDoubleClick(const RestartFacts &facts) {
	return facts.leftButton
		&& !facts.selecting
		&& facts.documentOpen
		&& facts.voiceOrRound
		&& !facts.viewOnce
		&& facts.currentTrack
		&& facts.seekable;
}

} // namespace Purple::MediaRules

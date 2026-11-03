/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Purple::MediaRules {

struct RestartFacts {
	bool leftButton = false;
	bool selecting = false;
	bool documentOpen = false;
	bool voiceOrRound = false;
	bool viewOnce = false;
	bool currentTrack = false;
	bool seekable = false;
};

[[nodiscard]] bool RestartsOnDoubleClick(const RestartFacts &facts);

} // namespace Purple::MediaRules

/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "rpl/producer.h"
#include "spellcheck/spellcheck_types.h"

#include <vector>

namespace Main {
class Session;
} // namespace Main

class History;

namespace Purple {

inline constexpr auto kTranslationRecognitionThreshold = 10;

[[nodiscard]] rpl::producer<bool> TranslationPremiumValue(
	not_null<Main::Session*> session);
[[nodiscard]] bool TranslationPremiumAvailableForSession(
	not_null<Main::Session*> session);
void UpdateTranslationOffer(
	not_null<History*> history,
	const std::vector<LanguageId> &recognized,
	int count,
	bool allLoaded);

} // namespace Purple

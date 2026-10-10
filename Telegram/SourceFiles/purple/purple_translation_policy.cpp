/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_translation_policy.h"

#include "base/algorithm.h"
#include "base/debug_log.h"
#include "base/flat_map.h"
#include "data/data_peer_values.h"
#include "history/history.h"
#include "main/main_session.h"
#include "purple/purple_config.h"
#include "rpl/combine.h"

#include <algorithm>

#include <QtCore/QString>

namespace Purple {
namespace {

constexpr auto kEnoughForTranslation = 6;

} // namespace

rpl::producer<bool> TranslationPremiumValue(
		not_null<Main::Session*> session) {
	using namespace rpl::mappers;
	return rpl::combine(
		Data::AmPremiumValue(session),
		LocalPremiumValue(),
		_1 || _2);
}

bool TranslationPremiumAvailableForSession(
		not_null<Main::Session*> session) {
	return session->premium() || LocalPremium();
}

void UpdateTranslationOffer(
		not_null<History*> history,
		const std::vector<LanguageId> &recognized,
		int count,
		bool allLoaded) {
	auto languages = base::flat_map<LanguageId, int>();
	for (const auto &language : recognized) {
		++languages[language];
	}
	using namespace base;
	constexpr auto p = &flat_multi_map_pair_type<LanguageId, int>::second;
	const auto threshold = (count > kTranslationRecognitionThreshold)
		? (count * kEnoughForTranslation / kTranslationRecognitionThreshold)
		: allLoaded
		? std::min(count, kEnoughForTranslation)
		: kEnoughForTranslation;
	const auto translatable = ranges::accumulate(
		languages,
		0,
		ranges::plus(),
		p);
	const auto was = history->translateOfferedFrom();
	// The threshold scales with all loaded messages and can rise while the
	// foreign-message count stays fixed. Keep an existing offer until those
	// messages fall below the minimum, rather than letting the bar flicker
	// while history loads.
	if (count >= kEnoughForTranslation) {
		if (translatable >= threshold) {
			history->translateOfferFrom(
				ranges::max_element(languages, ranges::less(), p)->first);
		} else if (!(was && translatable >= kEnoughForTranslation)) {
			history->translateOfferFrom({});
		}
	}
	const auto now = history->translateOfferedFrom();
	if (now != was) {
		LOG(("Purple: %1 translation of %2 - %3 of %4 messages recognised, "
			"threshold %5."
			).arg(QString::fromLatin1(now ? "offering" : "withdrawing")
			).arg(history->peer->name()
			).arg(translatable
			).arg(count
			).arg(threshold));
	} else if (!now && count >= kEnoughForTranslation && !languages.empty()) {
		LOG(("Purple: not offering translation of %1 - %2 of %3 messages "
			"recognised, need %4."
			).arg(history->peer->name()
			).arg(translatable
			).arg(count
			).arg(threshold));
	}
}

} // namespace Purple

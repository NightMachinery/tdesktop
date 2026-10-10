/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "purple/purple_suggestions.h"

#include "base/algorithm.h"
#include "data/data_folder.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "dialogs/dialogs_main_list.h"
#include "history/history.h"
#include "main/main_session.h"
#include "purple/purple_config.h"

#include <utility>

namespace Purple {
namespace {

constexpr auto kProbablyMaxChannels = 1000;

} // namespace

std::vector<not_null<PeerData*>> WithoutHiddenSuggestions(
		std::vector<not_null<PeerData*>> peers) {
	peers.erase(ranges::remove_if(peers, [](not_null<PeerData*> peer) {
		return HiddenFromSuggestions(peer->owner().history(peer));
	}), end(peers));
	return peers;
}

bool ChannelShownInSuggestions(not_null<History*> history) {
	return history->peer->isBroadcast()
		&& !HiddenFromSuggestions(history);
}

std::vector<not_null<History*>> MyChannels(
		not_null<Main::Session*> session) {
	auto channels = std::vector<not_null<History*>>();
	channels.reserve(kProbablyMaxChannels);
	const auto owner = &session->data();
	const auto add = [&](not_null<Dialogs::MainList*> list) {
		for (const auto &row : list->indexed()->all()) {
			if (const auto history = row->history()) {
				if (ChannelShownInSuggestions(history)) {
					channels.push_back(history);
				}
			}
		}
	};
	add(owner->chatsList());
	if (const auto folder = owner->folderLoaded(Data::Folder::kId)) {
		add(owner->chatsList(folder));
	}
	ranges::sort(channels, ranges::greater(), &History::chatListTimeId);
	return channels;
}

bool ShowRecommendedChannels(not_null<Main::Session*>) {
	return !Filtering() || ActiveSettings().suggestions.recommendedChannels;
}

bool RecommendedChannelsSelectable(
		not_null<Main::Session*> session,
		int count) {
	return count > 0 && ShowRecommendedChannels(session);
}

rpl::producer<> WithPresetChanges(rpl::producer<> changes) {
	return rpl::merge(std::move(changes), ActiveChanges());
}

} // namespace Purple

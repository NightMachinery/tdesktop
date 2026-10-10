/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "purple/purple_gate.h"
#include "rpl/producer.h"

#include <vector>

class History;
class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace Purple {

[[nodiscard]] std::vector<not_null<PeerData*>> WithoutHiddenSuggestions(
	std::vector<not_null<PeerData*>> peers);
[[nodiscard]] bool ChannelShownInSuggestions(not_null<History*> history);
[[nodiscard]] std::vector<not_null<History*>> MyChannels(
	not_null<Main::Session*> session);
[[nodiscard]] bool ShowRecommendedChannels(not_null<Main::Session*>);
[[nodiscard]] bool RecommendedChannelsSelectable(
	not_null<Main::Session*> session,
	int count);
[[nodiscard]] rpl::producer<> WithPresetChanges(rpl::producer<> changes);

template <typename Factory>
[[nodiscard]] auto RestartOnPresetChange(Factory factory) {
	return rpl::single(factory())
		| rpl::then(ActiveChanges() | rpl::map([=] {
			return factory();
		}))
		| rpl::flatten_latest();
}

} // namespace Purple

#pragma once

#include "purple/purple_settings.h"

namespace Purple {

inline int MediaReplyModeSelection(std::optional<MediaReplyMode> mode, bool selectedSet) {
	return mode ? int(*mode) : selectedSet ? 4 : 0;
}

inline bool MediaReplyRoundDisabled(const Settings &settings, const MediaReplyRow &row, bool selectedSet) {
	if (!selectedSet || row.roundFollow) return row.roundFollow.value_or(true);
	return ResolveMediaReply(settings, {PeerTokenKind::User, 0}, ChatKind::Private).roundFollow
		&& ResolveMediaReply(settings, {PeerTokenKind::Chat, 0}, ChatKind::Group).roundFollow
		&& ResolveMediaReply(settings, {PeerTokenKind::Channel, 0}, ChatKind::Channel).roundFollow;
}

} // namespace Purple

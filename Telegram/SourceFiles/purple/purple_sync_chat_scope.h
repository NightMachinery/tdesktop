#pragma once

#include "purple/purple_sync_local_state.h"

namespace Purple {

[[nodiscard]] inline bool DesktopSyncChatLedgerMatches(
		const SyncLocalState &state,
		PeerToken chat) {
	return PeerTokenText(chat).has_value()
		&& ((state.version == 1 && state.chat == PeerToken{} && chat == PeerToken{})
			|| (state.version == 2 && state.chat == chat));
}

}

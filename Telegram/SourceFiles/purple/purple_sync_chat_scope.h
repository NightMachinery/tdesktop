#pragma once

#include "purple/purple_sync_local_state.h"

#include <atomic>
#include <limits>
#include <memory>

namespace Purple {

class DesktopSyncChatReadStamp final {
public:
	[[nodiscard]] static DesktopSyncChatReadStamp Begin() {
		static auto counter = std::atomic<uint64_t>(0);
		static const auto domain = std::make_shared<const int>(0);
		auto sequence = counter.load();
		while (sequence != std::numeric_limits<uint64_t>::max()
			&& !counter.compare_exchange_weak(sequence, sequence + 1)) {
		}
		auto result = DesktopSyncChatReadStamp();
		if (sequence != std::numeric_limits<uint64_t>::max()) {
			result._sequence = sequence + 1;
			result._domain = domain;
		}
		return result;
	}
	[[nodiscard]] bool valid() const { return _domain && _sequence; }
	[[nodiscard]] bool after(const DesktopSyncChatReadStamp &other) const {
		return valid() && other.valid() && _domain == other._domain && _sequence > other._sequence;
	}

private:
	std::shared_ptr<const int> _domain;
	uint64_t _sequence = 0;

};

[[nodiscard]] inline bool DesktopSyncChatLedgerMatches(
		const SyncLocalState &state,
		PeerToken chat) {
	return PeerTokenText(chat).has_value()
		&& ((state.version == 1 && state.chat == PeerToken{} && chat == PeerToken{})
			|| (state.version == 2 && state.chat == chat));
}

}

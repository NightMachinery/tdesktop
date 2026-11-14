#include "purple/hooks/music_listening.h"

namespace Purple {

struct MusicListeningPrivacy::State {
	uint64_t revision = 0;
	bool enabled = true;
	bool discarding = false;
};

MusicListeningPrivacy::MusicListeningPrivacy(
		rpl::producer<bool> enabled,
		Fn<void()> discard)
: _state(std::make_shared<State>()) {
	std::move(enabled) | rpl::on_next([=, state = _state](bool value) {
		if (state->enabled == value) {
			return;
		}
		++state->revision;
		state->enabled = value;
		state->discarding = true;
		discard();
		state->discarding = false;
	}, _lifetime);
}

bool MusicListeningPrivacy::suppressed() const {
	return _state->enabled || _state->discarding;
}

Fn<bool()> MusicListeningPrivacy::reportAllowed() const {
	return [weak = std::weak_ptr<State>(_state), revision = _state->revision] {
		const auto state = weak.lock();
		return state
			&& (state->revision == revision)
			&& !state->enabled
			&& !state->discarding;
	};
}

} // namespace Purple

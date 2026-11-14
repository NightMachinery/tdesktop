#pragma once

#include "base/basic_types.h"

#include <rpl/producer.h>

#include <memory>

namespace Settings::Builder {
class SectionBuilder;
} // namespace Settings::Builder

namespace Purple {

[[nodiscard]] rpl::producer<bool> PrivateMusicListeningValue();
void AddPrivateMusicListeningSetting(
	::Settings::Builder::SectionBuilder &builder);

class MusicListeningPrivacy final {
public:
	MusicListeningPrivacy(rpl::producer<bool> enabled, Fn<void()> discard);

	[[nodiscard]] bool suppressed() const;
	[[nodiscard]] Fn<bool()> reportAllowed() const;

private:
	struct State;
	std::shared_ptr<State> _state;
	rpl::lifetime _lifetime;

};

} // namespace Purple

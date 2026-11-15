#pragma once

#include "base/basic_types.h"
#include "data/data_audio_msg_id.h"

namespace Window { class SessionController; }

namespace Purple {

[[nodiscard]] bool AccountPlaylistTrackClickable(
	not_null<Window::SessionController*> controller,
	AudioMsgId::Type type);
[[nodiscard]] bool OpenAccountPlaylistFromPlayer(
	not_null<Window::SessionController*> controller,
	AudioMsgId::Type type);

} // namespace Purple

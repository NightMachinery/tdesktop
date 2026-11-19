#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>
#include <optional>

namespace Main { class Session; }

namespace Purple {

struct PlaylistMessage;

struct MusicBookmark {
	QString token;
	QString title;
	QString performer;
};

[[nodiscard]] std::optional<MusicBookmark> CaptureMusicBookmark(
	not_null<Main::Session*> session,
	const PlaylistMessage &source);

} // namespace Purple

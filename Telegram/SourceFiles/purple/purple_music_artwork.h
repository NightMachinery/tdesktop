#pragma once

namespace Purple {

[[nodiscard]] auto MusicArtworkThumbnail(const auto &media) {
	const auto thumbnail = media.thumbnail();
	return thumbnail ? thumbnail : media.thumbnailInline();
}

} // namespace Purple

#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>
#include <memory>
#include <optional>

namespace Main { class Session; }

namespace Purple {

[[nodiscard]] std::shared_ptr<void> AcquirePlaylistOwnedTransferSlot();
void RegisterPlaylistDownloadJob(
	Main::Session *session, QString playlistId,
	std::weak_ptr<void> alive, Fn<void()> cancel);
void StopAccountPlaylistDownloads(
	Main::Session *session, const std::optional<QString> &playlistId = std::nullopt);
void InvalidatePlaylistDownloads();

} // namespace Purple

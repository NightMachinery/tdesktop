#pragma once

#include "purple/purple_archive_media_lifecycle.h"
#include <vector>
class HistoryItem;
namespace Main { class Session; }

namespace Purple {

void ObserveArchiveLifecycle(not_null<HistoryItem*> item,
	ArchiveMediaTransition transition = ArchiveMediaTransition::Expiry, bool wholeMessage = false);
void ObserveArchiveTtlExpiry(const std::vector<not_null<HistoryItem*>> &items);
void ObserveArchiveMediaReplacement(not_null<HistoryItem*> item, const MTPMessageMedia *replacement);
[[nodiscard]] bool ObserveArchiveExpiredReplacement(not_null<Main::Session*> session,
	const MTPMessage &message, int pts);

}

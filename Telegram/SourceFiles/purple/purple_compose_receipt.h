#pragma once

#include "purple/purple_archive_model.h"

namespace Purple {

[[nodiscard]] std::optional<ArchiveEvent> DesktopComposeAcknowledgement(
	const ArchiveMessageKey &opening,
	uint64 random,
	const ArchiveContent &expected,
	const MTPUpdates &updates);

}

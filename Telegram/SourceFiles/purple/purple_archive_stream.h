#pragma once

#include "purple/purple_archive_staging.h"
#include "purple/purple_desktop_archive_store.h"

namespace Purple {

class DesktopArchiveStream final {
public:
	DesktopArchiveStream(DesktopArchiveStore &store, ArchiveStagingBinding binding,
		std::shared_ptr<std::atomic<bool>> active = {});
	~DesktopArchiveStream();
	[[nodiscard]] bool Open();
	[[nodiscard]] ArchiveStagingStatus Write(uint64_t offset, const QByteArray &bytes);
	[[nodiscard]] bool Complete();
	[[nodiscard]] const ArchiveStagingHeader &header() const;

private:
	class Impl;
	std::unique_ptr<Impl> _impl;

};

}

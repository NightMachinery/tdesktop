#pragma once

#include "purple/purple_desktop_archive_store.h"
#include <mutex>

namespace Purple {

class DesktopArchiveJournal final {
  public:
	explicit DesktopArchiveJournal(DesktopArchiveJournalAccess access);
	[[nodiscard]] bool Open();
	[[nodiscard]] bool Append(ArchiveEvent event, int byteLimit = 0);
	[[nodiscard]] bool Attention(const QString &reason, int64 observedAt);
	[[nodiscard]] bool Drain(DesktopArchiveStore &store, int limit = 64);
	[[nodiscard]] QString failure() const;
	[[nodiscard]] const DesktopArchiveJournalAccess &access() const;

  private:
	[[nodiscard]] bool DrainPending(DesktopArchiveStore &store, int limit);
	DesktopArchiveJournalAccess _access;
	mutable std::mutex _mutex;
	QString _failure;
	uint64_t _sequence = 0;
	bool _ready = false;

};

}

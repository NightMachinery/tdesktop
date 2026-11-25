#pragma once

#include "base/basic_types.h"
#include "purple/purple_archive_v2_attention.h"
#include "purple/purple_archive_v2_store.h"

namespace Ui { class GenericBox; }
struct TextForMimeData;

namespace Purple::ArchiveV2 {

using ViewCurrent = std::function<bool()>;
using DecodeBody = std::function<TextForMimeData(const QByteArray &)>;

struct MediaViewActions {
	std::optional<std::pair<int64, int64>> saving;
	Fn<void(int64, int64)> openSaved;
};

void FillHistoryBox(not_null<Ui::GenericBox*> box, const HistoryPage &page,
	const DecodeBody &decode, ViewCurrent current, Fn<void(int64)> older, MediaViewActions media = {});
void FillTimelineBox(not_null<Ui::GenericBox*> box, const TimelinePage &page, bool deleted,
	const DecodeBody &decode, ViewCurrent current, Fn<void(TimelineCursor)> older);
void FillStorageBox(not_null<Ui::GenericBox*> box, const StorageSummary &summary,
	std::optional<int64> previousBytes, ViewCurrent current,
	Fn<void(CleanupKind)> reviewCleanup, Fn<void()> reviewPrevious);

void FillPreviousArchiveConfirm(not_null<Ui::GenericBox*> box, QString account,
	int64 reviewedBytes, ViewCurrent current, Fn<void()> confirm);

[[nodiscard]] ArchiveWarning LogIncident(Attention &attention, IncidentStage stage,
	QStringView reason, bool committed, std::optional<MediaKind> kind = {},
	uint64_t count = 1, uint64_t bytes = 0);

}

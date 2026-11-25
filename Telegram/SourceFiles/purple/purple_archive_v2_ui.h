#pragma once

#include "base/basic_types.h"
#include "purple/purple_archive_v2_attention.h"
#include "purple/purple_archive_v2_store.h"

namespace Ui { class GenericBox; }
struct TextForMimeData;

namespace Purple::ArchiveV2 {

using ViewCurrent = std::function<bool()>;
using DecodeBody = std::function<TextForMimeData(const QByteArray &)>;

template <typename Cursor, typename Page>
using LoadPage = Fn<void(Cursor, Fn<void(std::optional<Page>)>)>;
using LoadHistory = LoadPage<int64, HistoryPage>;
using LoadTimeline = LoadPage<TimelineCursor, TimelinePage>;

struct MediaViewActions {
	Fn<bool(int64, int64)> saving;
	Fn<void(int64, int64, QString)> openSaved;
};

void FillHistoryBox(not_null<Ui::GenericBox*> box, const HistoryPage &page,
	const DecodeBody &decode, ViewCurrent current, LoadHistory older, MediaViewActions media = {});
void FillTimelineBox(not_null<Ui::GenericBox*> box, const TimelinePage &page, bool deleted,
	const DecodeBody &decode, ViewCurrent current, LoadTimeline older,
	Fn<QString(const TimelineEntry&)> name = {}, Fn<void(MessageIdentity)> open = {});
[[nodiscard]] QString ArchiveFileName(const MediaStatus &media, int64 observedAt);
void FillStorageBox(not_null<Ui::GenericBox*> box, const std::optional<StorageSummary> &summary,
	std::optional<int64> previousBytes, ViewCurrent current,
	Fn<void(CleanupKind)> reviewCleanup, Fn<void()> reviewPrevious);

[[nodiscard]] QString ArchiveSize(int64 bytes);
[[nodiscard]] QString ArchiveCleanupText(QString result, int64 reclaimed);
void FillArchiveConfirm(not_null<Ui::GenericBox*> box, QString title, QString detail,
	ViewCurrent current, Fn<void()> confirm);
void FillPreviousArchiveConfirm(not_null<Ui::GenericBox*> box, QString account,
	int64 reviewedBytes, ViewCurrent current, Fn<void()> confirm);

[[nodiscard]] ArchiveWarning LogIncident(Attention &attention, IncidentStage stage,
	QStringView reason, bool committed, std::optional<MediaKind> kind = {},
	uint64_t count = 1, uint64_t bytes = 0);

}

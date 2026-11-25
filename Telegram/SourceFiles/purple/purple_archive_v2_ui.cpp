#include "purple/purple_archive_v2_ui.h"

#include "lang/lang_keys.h"
#include "logs.h"
#include "purple/purple_copy.h"
#include "styles/style_layers.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/buttons.h"

#include <QtCore/QDateTime>
#include <QtCore/QLocale>
#include <bit>

namespace Purple::ArchiveV2 {
namespace {

Ui::FlatLabel *Label(not_null<Ui::GenericBox*> box, QString text) {
	return box->addRow(object_ptr<Ui::FlatLabel>(box, std::move(text), st::boxLabel));
}

bool Ready(not_null<Ui::GenericBox*> box, const ViewCurrent &current) {
	if (current && current()) {
		box->setWidth(st::boxWideWidth);
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		return true;
	}
	return false;
}

QString Stamp(int64 seconds) {
	return QLocale().toString(QDateTime::fromSecsSinceEpoch(seconds), QLocale::ShortFormat);
}

QString Status(MessageStatus status) {
	switch (status) {
	case MessageStatus::Deleted: return tr::lng_purple_archive_deleted_badge(tr::now);
	case MessageStatus::Consumed: return u"Consumed"_q;
	case MessageStatus::Expired: return u"Expired"_q;
	case MessageStatus::Live: return {};
	}
	return {};
}

void Body(not_null<Ui::GenericBox*> box, const StoredBody &value, const DecodeBody &decode,
		const ViewCurrent &current) {
	const auto parsed = decode(value.body);
	auto text = Label(box, {});
	text->setMarkedText(parsed.rich);
	text->setSelectable(true);
	const auto markdown = CopyTextAsMarkdown(parsed);
	text->setContextMenuHook([text, current, markdown](Ui::FlatLabel::ContextMenuRequest request) {
		if (!current()) return;
		text->fillContextMenu(request);
		if (!markdown.isEmpty()) request.menu->addAction(u"Copy as Markdown"_q, [current, markdown] {
			if (current()) TextUtilities::SetClipboardText(TextForMimeData::Simple(markdown));
		});
	});
}

QString MediaReasonText(QStringView reason) {
	if (reason == u"over_allowance" || reason == u"quota_unreclaimable") return u"Storage allowance"_q;
	if (reason == u"expired") return u"Expired"_q;
	if (reason == u"over_file_limit") return u"File size limit"_q;
	if (reason == u"copy_incomplete") return u"Incomplete file"_q;
	if (reason == u"disk_full") return u"Disk full"_q;
	if (reason == u"store_busy") return u"Archive busy"_q;
	return u"Description only"_q;
}

QString MiB(int64 bytes) {
	return QLocale().toString(double(bytes) / kMiB, 'f', 2) + u" MiB"_q;
}

void Action(not_null<Ui::GenericBox*> box, QString label, const ViewCurrent &current, Fn<void()> callback) {
	if (!callback) return;
	const auto button = box->addRow(object_ptr<Ui::RoundButton>(box, rpl::single(std::move(label)), st::defaultBoxButton));
	button->setClickedCallback([current, callback = std::move(callback)] {
		if (current()) callback();
	});
}

}

void FillHistoryBox(not_null<Ui::GenericBox*> box, const HistoryPage &page,
		const DecodeBody &decode, ViewCurrent current, Fn<void(int64)> older, MediaViewActions media) {
	box->setTitle(tr::lng_purple_archive_history());
	if (!decode || !Ready(box, current)) return;
	if (const auto status = Status(page.status); !status.isEmpty()) Label(box, status);
	if (page.missingPreimage) Label(box, tr::lng_purple_archive_missing_preimage(tr::now));
	if (page.rows.empty() && !page.missingPreimage) Label(box, tr::lng_purple_archive_history_empty(tr::now));
	for (const auto &row : page.rows) {
		Label(box, Stamp(row.value.observedAt));
		Body(box, row.value, decode, current);
		for (const auto &entry : row.media) {
			if (entry.savedBytes > 0) {
				const auto label = u"Saved · "_q + MiB(entry.savedBytes);
				if (media.openSaved) {
					Action(box, label, current, [open = media.openSaved, seq = row.sequence, file = entry.ordinal] {
						open(seq, file);
					});
				} else {
					Label(box, label);
				}
			} else {
				const auto saving = media.saving == std::pair(row.sequence, entry.ordinal);
				Label(box, saving ? u"Saving…"_q : u"Not saved · "_q
					+ MediaReasonText(entry.reason));
			}
		}
	}
	if (page.before && older) {
		Action(box, tr::lng_purple_archive_older(tr::now), current, [older = std::move(older), cursor = page.before] {
			older(cursor);
		});
	}
}

void FillTimelineBox(not_null<Ui::GenericBox*> box, const TimelinePage &page, bool deleted,
		const DecodeBody &decode, ViewCurrent current, Fn<void(TimelineCursor)> older) {
	box->setTitle(deleted ? tr::lng_purple_archive_deleted() : tr::lng_purple_archive_history());
	if (!decode || !Ready(box, current)) return;
	if (page.rows.empty()) {
		Label(box, deleted ? tr::lng_purple_archive_deleted_empty(tr::now) : tr::lng_purple_archive_history_empty(tr::now));
	}
	for (const auto &row : page.rows) {
		const auto status = Status(row.status);
		Label(box, status.isEmpty() ? Stamp(row.observedAt) : status + u" · "_q + Stamp(row.observedAt));
		if (row.latest) Body(box, *row.latest, decode, current);
	}
	if (page.before && older) {
		Action(box, tr::lng_purple_archive_older(tr::now), current, [older = std::move(older), cursor = *page.before] {
			older(cursor);
		});
	}
}

void FillStorageBox(not_null<Ui::GenericBox*> box, const StorageSummary &summary,
		std::optional<int64> previousBytes, ViewCurrent current,
		Fn<void(CleanupKind)> reviewCleanup, Fn<void()> reviewPrevious) {
	box->setTitle(rpl::single(u"Archive storage"_q));
	if (!Ready(box, current)) return;
	for (const auto &[name, bytes] : std::array<std::pair<QString, int64>, 6>{
		{ { u"Active archive"_q, summary.active.physical() }, { u"Database"_q, summary.active.database },
			{ u"WAL"_q, summary.active.wal }, { u"Shared memory"_q, summary.active.sharedMemory },
			{ u"Support files"_q, summary.active.support }, { u"Reclaimed this session"_q, summary.active.reclaimed } },
	}) {
		Label(box, name + u" · "_q + MiB(bytes));
	}
	Label(box, u"Ordinary · %1 of %2 · %3 per file"_q.arg(
		MiB(summary.active.ordinary), MiB(summary.ordinaryCap), MiB(summary.ordinaryFileCap)));
	Label(box, u"Timed · %1 of %2"_q.arg(MiB(summary.active.timed), MiB(summary.timedCap)));
	Label(box, u"Pending deletion · "_q + (summary.pending ? MiB(*summary.pending) : u"Size unavailable"_q));
	if (summary.overLimit) Label(box, u"Over limit"_q);
	if (summary.lastTrimDay >= 0) Label(box, u"Last trim · "_q + Stamp(summary.lastTrimDay * 86400));
	if (reviewCleanup) {
		Action(box, u"Clear edit history…"_q, current, [reviewCleanup] { reviewCleanup(CleanupKind::EditHistory); });
		Action(box, u"Empty deleted messages…"_q, current, [reviewCleanup] { reviewCleanup(CleanupKind::DeletedMessages); });
	}
	if (previousBytes) {
		Label(box, u"Previous archive · "_q + MiB(*previousBytes));
		Action(box, u"Delete previous archive…"_q, current, std::move(reviewPrevious));
	}
}

void FillPreviousArchiveConfirm(not_null<Ui::GenericBox*> box, QString account, int64 reviewedBytes,
		ViewCurrent current, Fn<void()> confirm) {
	box->setTitle(rpl::single(u"Delete previous archive?"_q));
	if (!Ready(box, current)) return;
	Label(box, account + u" · "_q + MiB(reviewedBytes));
	Label(box, u"This permanently removes this account’s previous local archive."_q);
	auto confirmed = std::make_shared<bool>(false);
	box->addButton(rpl::single(u"Delete previous archive"_q), [box, current, confirm = std::move(confirm), confirmed] {
		if (!current() || !confirm || std::exchange(*confirmed, true)) return;
		box->closeBox();
		confirm();
	});
}

ArchiveWarning LogIncident(Attention &attention, IncidentStage stage, QStringView reason,
		bool committed, std::optional<MediaKind> kind, uint64_t count, uint64_t bytes) {
	const auto incident = attention.observe(stage, reason, committed);
	if (!incident.reason.isEmpty()) {
		const auto stageName = std::array{ u"startup", u"capture", u"whole_file", u"maintenance", u"cleanup" }[int(stage)];
		const auto media = kind ? std::array{ u"photo", u"video", u"round", u"voice", u"other" }[int(*kind)] : u"none";
		LOG(("Purple Archive: stage=%1 reason=%2 media=%3 count=%4 byte_power=%5.")
			.arg(QStringView(stageName), incident.reason, QStringView(media))
			.arg(qulonglong(count)).arg(std::bit_width(bytes)));
	}
	return incident.warning;
}

}

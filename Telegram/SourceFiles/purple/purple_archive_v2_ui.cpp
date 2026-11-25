#include "purple/purple_archive_v2_ui.h"

#include "lang/lang_keys.h"
#include "logs.h"
#include "purple/purple_copy.h"
#include "purple/purple_keys.h"
#include "styles/style_layers.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/buttons.h"

#include <QtCore/QDateTime>
#include <QtCore/QLocale>
#include <QtGui/QKeyEvent>
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
	case MessageStatus::Consumed: return tr::lng_purple_archive_v2_consumed(tr::now);
	case MessageStatus::Expired: return tr::lng_purple_archive_v2_expired(tr::now);
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
		if (!markdown.isEmpty()) request.menu->addAction(tr::lng_purple_copy_as_markdown(tr::now), [current, markdown] {
			if (current()) TextUtilities::SetClipboardText(TextForMimeData::Simple(markdown));
		});
	});
}

QString MediaReasonText(QStringView reason) {
	if (reason == u"over_allowance" || reason == u"quota_unreclaimable") return tr::lng_purple_archive_v2_allowance(tr::now);
	if (reason == u"expired") return tr::lng_purple_archive_v2_expired(tr::now);
	if (reason == u"over_file_limit") return tr::lng_purple_archive_v2_file_limit(tr::now);
	if (reason == u"copy_incomplete") return tr::lng_purple_archive_v2_incomplete(tr::now);
	if (reason == u"disk_full") return tr::lng_purple_archive_v2_disk_full(tr::now);
	if (reason == u"store_busy") return tr::lng_purple_archive_v2_busy(tr::now);
	return tr::lng_purple_archive_v2_description(tr::now);
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
				const auto label = tr::lng_purple_archive_v2_saved(tr::now, lt_size, MiB(entry.savedBytes));
				if (media.openSaved) {
					Action(box, label, current, [open = media.openSaved, seq = row.sequence, file = entry.ordinal] {
						open(seq, file);
					});
				} else {
					Label(box, label);
				}
			} else {
				const auto saving = media.saving == std::pair(row.sequence, entry.ordinal);
				Label(box, saving ? tr::lng_purple_archive_v2_saving(tr::now)
					: tr::lng_purple_archive_v2_not_saved(tr::now, lt_reason, MediaReasonText(entry.reason)));
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
	box->setTitle(tr::lng_purple_archive_v2_storage());
	if (!Ready(box, current)) return;
	const auto &active = summary.active;
	const auto overhead = std::max<int64>(0, active.physical() - active.ordinary - active.timed);
	Label(box, tr::lng_purple_archive_v2_active(tr::now, lt_size, MiB(active.physical())));
	Label(box, tr::lng_purple_archive_v2_ordinary(tr::now, lt_size, MiB(active.ordinary),
		lt_total, MiB(summary.ordinaryCap), lt_limit, MiB(summary.ordinaryFileCap)));
	Label(box, tr::lng_purple_archive_v2_timed(tr::now, lt_size, MiB(active.timed), lt_total, MiB(summary.timedCap)));
	Label(box, tr::lng_purple_archive_v2_pending(tr::now, lt_size,
		summary.pending ? MiB(*summary.pending) : tr::lng_purple_archive_v2_unknown_size(tr::now)));
	Label(box, tr::lng_purple_archive_v2_overhead(tr::now, lt_size, MiB(overhead), lt_total, MiB(summary.overheadCap)));
	if (summary.overLimit) Label(box, tr::lng_purple_archive_v2_over_limit(tr::now));
	LOG(("Purple Archive: event=storage db_bytes=%1 wal_bytes=%2 shm_bytes=%3 support_bytes=%4.")
		.arg(active.database).arg(active.wal).arg(active.sharedMemory).arg(active.support));
	if (reviewCleanup) {
		Action(box, tr::lng_purple_archive_v2_edit_clear(tr::now), current, [reviewCleanup] { reviewCleanup(CleanupKind::EditHistory); });
		Action(box, tr::lng_purple_archive_v2_deleted_clear(tr::now), current, [reviewCleanup] { reviewCleanup(CleanupKind::DeletedMessages); });
	}
	if (previousBytes) {
		Label(box, tr::lng_purple_archive_v2_previous(tr::now, lt_size, MiB(*previousBytes)));
		Action(box, tr::lng_purple_archive_v2_previous_review(tr::now), current, std::move(reviewPrevious));
	}
}

void FillPreviousArchiveConfirm(not_null<Ui::GenericBox*> box, QString account, int64 reviewedBytes,
		ViewCurrent current, Fn<void()> confirm) {
	box->setTitle(tr::lng_purple_archive_v2_previous_title());
	if (!current || !current()) return;
	box->setWidth(st::boxWideWidth);
	Label(box, tr::lng_purple_archive_v2_previous_account(tr::now, lt_account, account, lt_size, MiB(reviewedBytes)));
	Label(box, tr::lng_purple_archive_v2_previous_detail(tr::now));
	if (ModalKeysEnabled()) Label(box, tr::lng_purple_archive_v2_confirm_keys(tr::now));
	const auto submit = [box, current, confirm = std::move(confirm), used = std::make_shared<bool>(false)] {
		if (!current() || !confirm || std::exchange(*used, true)) return;
		box->closeBox();
		confirm();
	};
	box->addButton(tr::lng_purple_archive_v2_previous_button(), submit, st::attentionBoxButton);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	box->events() | rpl::on_next([=](not_null<QEvent*> event) {
		if (event->type() != QEvent::KeyPress || !ModalKeysEnabled()) return;
		const auto key = static_cast<QKeyEvent*>(event.get());
		if (key->modifiers() || key->isAutoRepeat()) return;
		if (key->key() == Qt::Key_Y) {
			event->accept();
			submit();
		} else if (key->key() == Qt::Key_N || key->key() == Qt::Key_Escape) {
			event->accept();
			box->closeBox();
		}
	}, box->lifetime());
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

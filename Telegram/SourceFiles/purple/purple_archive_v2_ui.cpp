#include "purple/purple_archive_v2_ui.h"

#include "base/event_filter.h"
#include "lang/lang_keys.h"
#include "logs.h"
#include "purple/purple_copy.h"
#include "purple/purple_keys.h"
#include "styles/style_layers.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/style/style_core_direction.h"
#include "ui/text/format_values.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/buttons.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMimeDatabase>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QTextEdit>
#include <bit>

namespace Purple::ArchiveV2 {
namespace {

constexpr auto kPreviewLines = 3;

QColor NoteColor() {
	auto color = st::windowFg->c;
	color.setAlphaF(.68);
	return color;
}

Ui::FlatLabel *Label(not_null<Ui::GenericBox*> box, QString text, bool note = false, bool title = false) {
	static const auto headingStyle = [] {
		auto style = st::defaultSubsectionTitle;
		style.textFg = st::windowBoldFg;
		return style;
	}();
	const auto &labelStyle = title ? headingStyle : note ? st::defaultSubTextLabel : st::boxLabel;
	auto child = object_ptr<Ui::FlatLabel>(box, std::move(text), labelStyle);
	if (note) child->setTextColorOverride(NoteColor());
	if (!st::rtl()) return box->addRow(std::move(child), st::boxRowPadding, style::al_justify);
	// Keep the full-width column while anchoring short native labels at the start in RTL.
	const auto row = box->addRow(object_ptr<Ui::VerticalLayout>(box), st::boxRowPadding, style::al_justify);
	return row->add(std::move(child));
}

void DrawLine(Painter &p, const style::font &font, const QString &text, QRect rect, bool end = false) {
	p.setFont(font);
	p.setLayoutDirection(style::LayoutDirection());
	const auto align = (st::rtl() != end) ? Qt::AlignRight : Qt::AlignLeft;
	p.drawText(rect, align | Qt::AlignAbsolute | Qt::AlignVCenter | Qt::TextSingleLine | Qt::TextDontClip,
		font->elided(text, rect.width()));
}

void SetArchiveWidth(not_null<Ui::GenericBox*> box) {
	box->setWidth(st::boxWideWidth);
	// GenericBox's margin wrapper positions its child before resizing in RTL.
	if (st::rtl()) box->setInitScrollCallback([box] { box->verticalLayout()->move(0, 0); });
}

bool Ready(not_null<Ui::GenericBox*> box, const ViewCurrent &current) {
	if (!current || !current()) return false;
	SetArchiveWidth(box);
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	return true;
}

QString Stamp(int64 seconds) {
	return langDateTime(QDateTime::fromSecsSinceEpoch(seconds));
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
	text->setTryMakeSimilarLines(false);
	text->setMarkedText(parsed.rich);
	text->setSelectable(true);
	base::install_event_filter(text, [box](not_null<QEvent*> event) {
		using Result = base::EventFilterResult;
		if (event->type() != QEvent::KeyPress || !ModalKeysEnabled()) return Result::Continue;
		const auto key = static_cast<QKeyEvent*>(event.get());
		if (key->key() != Qt::Key_Escape || key->modifiers()) return Result::Continue;
		box->setFocus();
		return Result::Cancel;
	});
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
	if (reason == u"copy_incomplete" || reason == u"transfer_failed") return tr::lng_purple_archive_v2_incomplete(tr::now);
	if (reason == u"disk_full") return tr::lng_purple_archive_v2_disk_full(tr::now);
	if (reason == u"media_queue_full") return tr::lng_purple_archive_v2_queue_full(tr::now);
	if (reason == u"store_busy") return tr::lng_purple_archive_v2_busy(tr::now);
	return tr::lng_purple_archive_v2_description(tr::now);
}

template <typename Cursor, typename Page, typename Append>
void Pages(not_null<Ui::GenericBox*> box, QString label, const Page &page, ViewCurrent current,
		LoadPage<Cursor, Page> load, Append append) {
	if (!page.before || !load) return;
	const auto cursor = box->lifetime().make_state<std::optional<Cursor>>(page.before);
	const auto button = box->addLeftButton(rpl::single(std::move(label)));
	button->setFocusPolicy(Qt::StrongFocus);
	button->setClickedCallback([=] {
		if (!*cursor || !current() || button->isDisabled()) return;
		button->setDisabled();
		load(**cursor, [=, guard = QPointer<Ui::GenericBox>(box.get())](std::optional<Page> next) {
			if (!guard || !button || !current()) return;
			button->setDisabled(false);
			if (!next) return;
			const auto y = box->scrollTop();
			append(*next);
			box->scrollToY(y);
			*cursor = next->before;
			if (!*cursor) {
				if (button->hasFocus()) box->setFocus();
				button->hide();
				box->updateButtonsGeometry();
			}
		});
	});
}

class ArchiveRow final : public Ui::RippleButton {
public:
	ArchiveRow(QWidget *parent, QString name, QString time, QString preview)
	: RippleButton(parent, st::defaultRippleAnimationBgOver)
	, _name(std::move(name))
	, _time(std::move(time))
	, _preview(st::boxTextStyle, preview, kDefaultTextOptions, 80) {
		setFocusPolicy(Qt::StrongFocus);
		setIsListItem(!_time.isEmpty());
		setAccessibleName(_time.isEmpty() ? _name : _name + u", "_q + _time + u", "_q + preview);
	}
	bool match(const QString &query) const {
		return query.isEmpty() || accessibleName().contains(query, Qt::CaseInsensitive);
	}
	void filter(const QString &query) {
		_match = match(query);
		resizeToWidth(width());
		setVisible(_match);
	}

protected:
	int resizeGetHeight(int width) override {
		if (!_match || _time.isEmpty()) return _match ? 38 : 0;
		return 27 + st::semiboldFont->height
			+ std::min(_preview.countHeight(std::max(1, width - 48)), kPreviewLines * st::boxTextFont->height);
	}
	void focusInEvent(QFocusEvent *e) override { RippleButton::focusInEvent(e); update(); }
	void focusOutEvent(QFocusEvent *e) override { RippleButton::focusOutEvent(e); update(); }
	void paintEvent(QPaintEvent *) override {
		auto p = Painter(this);
		if (isOver() || hasFocus()) p.fillRect(rect(), st::windowBgOver);
		paintRipple(p, 0, 0);
		if (hasFocus()) p.fillRect(style::rtlrect(0, 2, 2, height() - 4, width()), st::windowActiveTextFg);
		const auto pad = st::boxRowPadding.left();
		const auto available = width() - 2 * pad;
		if (_time.isEmpty()) {
			p.setPen(st::windowActiveTextFg);
			DrawLine(p, st::boxTextFont, _name, QRect(pad, 0, available, height()));
			return;
		}
		const auto timeWidth = std::min(available / 2, st::normalFont->width(_time) + 2);
		p.setPen(NoteColor());
		DrawLine(p, st::normalFont, _time, style::rtlrect(width() - pad - timeWidth, 4, timeWidth, 22, width()), true);
		p.setPen(st::windowFg);
		DrawLine(p, st::semiboldFont, _name, style::rtlrect(pad, 4, available - timeWidth - 12, 22, width()));
		_preview.draw(p, { .position = { pad, 15 + st::semiboldFont->height }, .availableWidth = available,
			.elisionLines = kPreviewLines, .useFullWidth = true });
	}

private:
	QString _name, _time;
	Ui::Text::String _preview;
	bool _match = true;
};

void Action(not_null<Ui::GenericBox*> box, QString label, const ViewCurrent &current, Fn<void()> callback) {
	if (!callback) return;
	const auto button = box->addRow(object_ptr<ArchiveRow>(box, std::move(label), QString(), QString()),
		style::margins(), style::al_justify);
	button->setClickedCallback([current, callback = std::move(callback)] { if (current()) callback(); });
}

}

QString ArchiveSize(int64 bytes) {
	const auto value = Ui::FormatSizeText(bytes);
	return st::rtl() ? u'\u2066' + value + u'\u2069' : value;
}

QString ArchiveCleanupText(QString result, int64 reclaimed) {
	return reclaimed > 0 ? result + u' ' + tr::lng_purple_archive_v2_reclaimed(tr::now, lt_size, ArchiveSize(reclaimed))
		: result;
}

QString ArchiveFileName(const MediaStatus &media, int64 observedAt) {
	const auto object = QJsonDocument::fromJson(media.description).object();
	auto name = object.value(u"name"_q).toString().replace(u'\\', u'/').section(u'/', -1).trimmed();
	name.removeIf([](QChar ch) { return ch.isNull() || ch.category() == QChar::Other_Control; });
	if (!name.isEmpty() && name != u"." && name != u"..") return name;
	const auto mime = QMimeDatabase().mimeTypeForName(object.value(u"mime"_q).toString());
	const auto kinds = std::array{ u"photo", u"video", u"video-message", u"voice-message", u"file" };
	const auto fallback = std::array{ u"jpg", u"mp4", u"mp4", u"ogg", u"bin" };
	const auto index = int(media.kind);
	const auto suffix = mime.preferredSuffix();
	return QString(kinds[index]) + u'-' + QDateTime::fromSecsSinceEpoch(observedAt).date().toString(Qt::ISODate)
		+ u'.' + (suffix.isEmpty() || mime.isDefault() ? QString(fallback[index]) : suffix);
}

void FillHistoryBox(not_null<Ui::GenericBox*> box, const HistoryPage &page,
		const DecodeBody &decode, ViewCurrent current, LoadHistory older, MediaViewActions media) {
	box->setTitle(tr::lng_purple_archive_v2_history_title());
	if (!decode || !Ready(box, current)) return;
	if (const auto status = Status(page.status); !status.isEmpty()) {
		Label(box, page.statusAt ? status + u" · "_q + Stamp(page.statusAt) : status, true);
	}
	if (page.missingPreimage) Label(box, tr::lng_purple_archive_missing_preimage(tr::now), true);
	if (page.rows.empty() && !page.missingPreimage) Label(box, tr::lng_purple_archive_history_empty(tr::now), true);
	const auto append = [=, status = page.status](const HistoryPage &chunk, bool firstPage) {
		for (const auto &row : chunk.rows) {
			if (!(firstPage && &row == &chunk.rows.front())) {
				const auto rule = box->addRow(object_ptr<Ui::FixedHeightWidget>(box, st::lineWidth),
					st::boxRowPadding, style::al_justify);
				rule->paintOn([=](QPainter &p) { p.fillRect(rule->rect(), st::windowBgOver); });
			}
			box->addSkip(12);
			const auto currentVersion = firstPage && &row == &chunk.rows.front() && status == MessageStatus::Live;
			const auto tag = currentVersion ? tr::lng_purple_archive_v2_current(tr::now)
				: row.sequence > 1 ? tr::lng_purple_archive_v2_edited(tr::now) : QString();
			const auto date = Stamp(row.value.observedAt);
			Label(box, tag.isEmpty() ? date : tag + u" · "_q + date, true);
			Body(box, row.value, decode, current);
			for (const auto &entry : row.media) {
				const auto name = ArchiveFileName(entry, row.value.observedAt);
				if (entry.savedBytes > 0 && media.openSaved) {
					const auto label = tr::lng_purple_archive_v2_export(tr::now, lt_name, st::rtl() ? st::wrap_rtl(name) : name, lt_size, ArchiveSize(entry.savedBytes));
					Action(box, label, current, [open = media.openSaved, seq = row.sequence, file = entry.ordinal, name] {
						open(seq, file, name);
					});
				} else {
					const auto saving = media.saving && media.saving(row.sequence, entry.ordinal);
					Label(box, name + u" · "_q + (saving ? tr::lng_purple_archive_v2_saving(tr::now)
						: tr::lng_purple_archive_v2_not_saved(tr::now, lt_reason, MediaReasonText(entry.reason))), true);
				}
			}
			box->addSkip(12);
		}
	};
	append(page, true);
	Pages(box, tr::lng_purple_archive_older(tr::now), page, current, std::move(older),
		[=](const HistoryPage &chunk) { append(chunk, false); });
}

void FillTimelineBox(not_null<Ui::GenericBox*> box, const TimelinePage &page, bool deleted,
		const DecodeBody &decode, ViewCurrent current, LoadTimeline older,
		Fn<QString(const TimelineEntry&)> name, Fn<void(MessageIdentity)> open) {
	box->setTitle(deleted ? tr::lng_purple_archive_deleted() : tr::lng_purple_archive_history());
	if (!decode || !Ready(box, current)) return;
	if (page.rows.empty()) {
		Label(box, deleted ? tr::lng_purple_archive_deleted_empty(tr::now) : tr::lng_purple_archive_history_empty(tr::now), true);
		return;
	}
	const auto label = tr::lng_purple_archive_v2_filter(tr::now) + (ModalKeysEnabled() ? u"  /"_q : QString());
	static const auto rtlField = [] {
		auto style = st::defaultInputField;
		style.placeholderScale = 0;
		style.placeholderAlign = style::al_topright;
		style.textAlign = style::al_topright;
		return style;
	}();
	const auto filter = box->addRow(object_ptr<Ui::InputField>(box, st::rtl() ? rtlField : st::defaultInputField,
		Ui::InputField::Mode::SingleLine, rpl::single(label)), st::boxRowPadding, style::al_justify);
	filter->setAccessibleName(label);
	box->addSkip(8);
	const auto rows = box->lifetime().make_state<std::vector<ArchiveRow*>>();
	const auto anchor = box->lifetime().make_state<QPointer<ArchiveRow>>();
	const auto beforeFilter = box->lifetime().make_state<QPointer<ArchiveRow>>();
	const auto empty = box->lifetime().make_state<Ui::FlatLabel*>(nullptr);
	const auto refilter = [=] {
		const auto query = filter->getLastText().simplified();
		auto found = false;
		for (const auto row : *rows) {
			row->filter(query);
			found |= row->match(query);
		}
		if (!found && !*empty) *empty = Label(box, tr::lng_purple_archive_v2_no_matches(tr::now), true);
		else if (found && *empty) {
			const auto note = std::exchange(*empty, nullptr);
			delete (st::rtl() ? note->parentWidget() : note);
		}
	};
	const auto focus = [=](int step) {
		auto visible = std::vector<ArchiveRow*>();
		for (const auto row : *rows) if (!row->isHidden()) visible.push_back(row);
		if (visible.empty()) return;
		const auto at = std::ranges::find(visible, anchor->data());
		const auto index = at == end(visible) ? 0 : std::clamp(int(at - begin(visible)) + step, 0, int(visible.size()) - 1);
		const auto row = visible[index];
		row->setFocus();
		box->scrollToWidget(row);
	};
	const auto keys = [=](not_null<QEvent*> event) {
		using Result = base::EventFilterResult;
		if (event->type() != QEvent::KeyPress) return Result::Continue;
		const auto key = static_cast<QKeyEvent*>(event.get());
		if (key->modifiers() & ~Qt::KeypadModifier) return Result::Continue;
		const auto input = filter->rawTextEdit()->hasFocus();
		const auto modal = ModalKeysEnabled();
		const auto code = key->key();
		if (code == Qt::Key_Escape && (input || !filter->getLastText().isEmpty())) {
			filter->setText({});
			if (*beforeFilter) *anchor = *beforeFilter;
			refilter();
			focus(0);
		} else if (!input && modal && code == Qt::Key_Slash) {
			filter->setFocus();
			box->scrollToWidget(filter);
		} else if (!input && (code == Qt::Key_Up || (modal && code == Qt::Key_K))) {
			focus(-1);
		} else if (code == Qt::Key_Down || (!input && modal && code == Qt::Key_J)) {
			focus(input ? 0 : 1);
		} else if (input && (code == Qt::Key_Return || code == Qt::Key_Enter)) {
			if (!key->isAutoRepeat()) focus(0);
		} else {
			return Result::Continue;
		}
		return Result::Cancel;
	};
	base::install_event_filter(filter->rawTextEdit(), [=](not_null<QEvent*> event) {
		if (event->type() == QEvent::FocusIn && filter->getLastText().isEmpty()) *beforeFilter = *anchor;
		return keys(event);
	});
	base::install_event_filter(box, keys);
	filter->changes() | rpl::on_next(refilter, filter->lifetime());
	const auto append = [=](const TimelinePage &chunk) {
		for (const auto &entry : chunk.rows) {
			auto preview = entry.latest ? decode(entry.latest->body).rich.text : QString();
			if (preview.isEmpty()) preview = entry.latest ? tr::lng_purple_archive_v2_media_preview(tr::now)
				: tr::lng_purple_archive_missing_preimage(tr::now);
			const auto title = name ? name(entry) : QString();
			const auto row = box->addRow(object_ptr<ArchiveRow>(box,
				title.isEmpty() ? tr::lng_purple_archive_v2_unknown_chat(tr::now) : title,
				Stamp(entry.observedAt), preview.left(800)), style::margins(), style::al_justify);
			row->setClickedCallback([=, item = entry.identity] { if (current() && open) open(item); });
			base::install_event_filter(row, [=](not_null<QEvent*> event) {
				if (event->type() == QEvent::FocusIn) *anchor = row;
				return keys(event);
			});
			rows->push_back(row);
		}
		refilter();
	};
	append(page);
	box->setFocusCallback([=] { focus(0); });
	Pages(box, tr::lng_purple_archive_v2_older_messages(tr::now), page, current, std::move(older), append);
}

void FillStorageBox(not_null<Ui::GenericBox*> box, const std::optional<StorageSummary> &summary,
		std::optional<int64> previousBytes, ViewCurrent current,
		Fn<void(CleanupKind)> reviewCleanup, Fn<void()> reviewPrevious) {
	box->setTitle(tr::lng_purple_archive_v2_storage());
	if (!Ready(box, current)) return;
	const auto heading = [=](QString title) {
		box->addSkip(12);
		Label(box, std::move(title), false, true);
		box->addSkip(8);
	};
	const auto value = [](int64 used, int64 cap) {
		return tr::lng_purple_archive_v2_usage(tr::now, lt_size, ArchiveSize(used), lt_total, ArchiveSize(cap));
	};
	const auto row = [=](QString label, QString amount) {
		const auto widget = box->addRow(object_ptr<Ui::RpWidget>(box), st::boxRowPadding, style::al_justify);
		widget->resize(1, 39);
		widget->setAccessibleName(label + u", "_q + amount);
		widget->paintRequest() | rpl::on_next([=] {
			auto p = Painter(widget);
			const auto w = widget->width();
			const auto endWidth = std::min(w / 2, st::normalFont->width(amount) + 2);
			const auto left = st::rtl() ? Qt::AlignRight : Qt::AlignLeft;
			p.setFont(st::boxTextFont);
			p.setPen(st::windowFg);
			p.drawText(style::rtlrect(0, 0, w - endWidth - 10, 39, w),
				left | Qt::AlignAbsolute | Qt::AlignVCenter | Qt::TextWordWrap | Qt::TextDontClip, label);
			p.setPen(NoteColor());
			DrawLine(p, st::normalFont, amount, style::rtlrect(w - endWidth, 0, endWidth, 39, w), true);
		}, widget->lifetime());
	};
	if (summary) {
		const auto &active = summary->active;
		const auto overhead = std::max<int64>(0, active.physical() - active.ordinary - active.timed);
		heading(tr::lng_purple_archive_v2_used(tr::now, lt_size, ArchiveSize(active.physical())));
		row(tr::lng_purple_archive_v2_ordinary(tr::now), value(active.ordinary, summary->ordinaryCap));
		row(tr::lng_purple_archive_v2_timed(tr::now), value(active.timed, summary->timedCap));
		if (overhead) row(tr::lng_purple_archive_v2_overhead(tr::now), value(overhead, summary->overheadCap));
		if (summary->pending != 0) row(tr::lng_purple_archive_v2_pending(tr::now), summary->pending
			? ArchiveSize(*summary->pending) : tr::lng_purple_archive_v2_unknown_size(tr::now));
		Label(box, tr::lng_purple_archive_v2_file_cap(tr::now, lt_size, ArchiveSize(summary->ordinaryFileCap)), true);
		if (summary->overLimit) Label(box, tr::lng_purple_archive_v2_over_limit(tr::now), true);
	} else {
		Label(box, tr::lng_purple_archive_unavailable(tr::now), true);
	}
	if (summary && reviewCleanup) {
		heading(tr::lng_purple_archive_v2_clean_up(tr::now));
		Action(box, tr::lng_purple_archive_v2_edit_clear(tr::now), current, [reviewCleanup] { reviewCleanup(CleanupKind::EditHistory); });
		Action(box, tr::lng_purple_archive_v2_deleted_clear(tr::now), current, [reviewCleanup] { reviewCleanup(CleanupKind::DeletedMessages); });
	}
	if (reviewPrevious) {
		heading(tr::lng_purple_archive_v2_previous(tr::now));
		if (previousBytes) row(tr::lng_purple_archive_v2_previous_size(tr::now), ArchiveSize(*previousBytes));
		Action(box, tr::lng_purple_archive_v2_previous_review(tr::now), current, std::move(reviewPrevious));
	}
	box->addSkip(12);
}

void FillArchiveConfirm(not_null<Ui::GenericBox*> box, QString title, QString detail,
		ViewCurrent current, Fn<void()> confirm) {
	box->setTitle(rpl::single(std::move(title)));
	if (!current || !current()) return;
	SetArchiveWidth(box);
	Label(box, std::move(detail));
	box->addSkip(12);
	const auto submit = [box, current, confirm = std::move(confirm), used = std::make_shared<bool>(false)] {
		if (!current() || !confirm || std::exchange(*used, true)) return;
		const auto callback = confirm;
		box->closeBox();
		callback();
	};
	const auto remove = box->addButton(ModalKeysEnabled() ? tr::lng_purple_archive_v2_confirm_delete() : tr::lng_context_delete_msg(),
		submit, st::attentionBoxButton);
	const auto cancel = box->addButton(ModalKeysEnabled() ? tr::lng_purple_archive_v2_confirm_cancel() : tr::lng_cancel(),
		[=] { box->closeBox(); });
	box->setFocusCallback([cancel] { if (cancel) cancel->setFocus(); });
	const auto confirmKeys = [=](not_null<QEvent*> event) {
		using Result = base::EventFilterResult;
		if (event->type() != QEvent::KeyPress || !ModalKeysEnabled()) return Result::Continue;
		const auto key = static_cast<QKeyEvent*>(event.get());
		if (key->modifiers() || key->isAutoRepeat()) return Result::Continue;
		if (key->key() == Qt::Key_Y) submit();
		else if (key->key() == Qt::Key_Escape || key->key() == Qt::Key_N) box->closeBox();
		else return Result::Continue;
		return Result::Cancel;
	};
	base::install_event_filter(box, confirmKeys);
	if (remove) base::install_event_filter(remove.data(), confirmKeys);
	if (cancel) base::install_event_filter(cancel.data(), confirmKeys);
}

void FillPreviousArchiveConfirm(not_null<Ui::GenericBox*> box, QString account, int64 reviewedBytes,
		ViewCurrent current, Fn<void()> confirm) {
	FillArchiveConfirm(box, tr::lng_purple_archive_v2_previous_title(tr::now),
		tr::lng_purple_archive_v2_previous_account(tr::now, lt_account, account, lt_size, ArchiveSize(reviewedBytes))
			+ '\n' + tr::lng_purple_archive_v2_previous_detail(tr::now),
		std::move(current), std::move(confirm));
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

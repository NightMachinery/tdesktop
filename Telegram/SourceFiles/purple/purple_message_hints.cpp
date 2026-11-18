#include "purple/purple_message_hints.h"

#include "config.h"
#include "data/components/scheduled_messages.h"
#include "data/data_groups.h"
#include "data/data_session.h"
#include "history/history_inner_widget.h"
#include "history/history_item.h"
#include "history/history_item_text.h"
#include "history/history_widget.h"
#include "history/view/history_view_element.h"
#include "history/view/history_view_list_widget.h"
#include "history/view/media/history_view_media.h"
#include "iv/iv_rich_message_html_export.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "ui/round_rect.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/elastic_scroll.h"
#include "window/window_session_controller.h"

#include "styles/palette.h"
#include "styles/style_chat.h"
#include "styles/style_widgets.h"

#include <QtGui/QPainter>

#include <string_view>

namespace Purple {
namespace {

constexpr auto kHintLetters = std::u16string_view(u"asdfghjkl");

QString HintLabel(size_t index, int digits) {
	auto result = QString(digits, QChar());
	for (auto i = digits; i > 0; --i) {
		result[i - 1] = QChar(kHintLetters[index % kHintLetters.size()]);
		index /= kHintLetters.size();
	}
	return result;
}

QSize LabelSize(const QString &label) {
	const auto &font = st::defaultToast.style.font;
	const auto &padding = st::msgServicePadding;
	return QSize(
		font->width(label) + padding.left() + padding.right(),
		font->height + padding.top() + padding.bottom());
}

} // namespace

class MessageHints::Overlay final : public Ui::RpWidget {
public:
	Overlay(not_null<Ui::RpWidget*> parent, const MessageHints &hints)
	: Ui::RpWidget(parent)
	, _hints(hints)
	, _background(st::defaultToast.radius, st::toastBg) {
		setFocusPolicy(Qt::NoFocus);
		setAttribute(Qt::WA_TransparentForMouseEvents);
	}

protected:
	void paintEvent(QPaintEvent *) override {
		auto p = QPainter(this);
		p.setFont(st::defaultToast.style.font);
		p.setPen(st::toastFg->c);
		for (const auto &entry : _hints._entries) {
			if (!entry.label.startsWith(_hints._prefix)) {
				continue;
			}
			const auto anchor = entry.rect.translated(-geometry().topLeft());
			const auto size = LabelSize(entry.label);
			const auto x = style::RightToLeft()
				? anchor.right() + 1 - size.width()
				: anchor.x();
			const auto box = QRect(
				QPoint(std::clamp(x, 0, std::max(width() - size.width(), 0)),
					anchor.y()),
				size);
			_background.paint(p, box);
			p.drawText(box, Qt::AlignCenter, entry.label);
		}
	}

private:
	const MessageHints &_hints;
	Ui::RoundRect _background;
};

MessageHints::MessageHints() = default;

MessageHints::~MessageHints() {
	clear();
}

bool MessageHints::active() const {
	return _overlay != nullptr;
}

Ui::RpWidget *MessageHints::Target::widget() const {
	return main ? static_cast<Ui::RpWidget*>(main) : section;
}

Main::Session &MessageHints::Target::session() const {
	return main ? main->session() : section->session();
}

MessageHints::Target MessageHints::target() const {
	return {
		.main = dynamic_cast<HistoryInner*>(_view.data()),
		.section = dynamic_cast<HistoryView::ListWidget*>(_view.data()),
		.scheduled = _scheduled,
	};
}

QRect MessageHints::clip() const {
	const auto view = target();
	const auto top = view.main ? view.main->_visibleAreaTop : view.section->_visibleTop;
	const auto bottom = view.main
		? view.main->_visibleAreaBottom
		: view.section->_visibleBottom;
	return QRect(0, top, _view->width(), bottom - top).intersected(_view->rect());
}

bool MessageHints::eligible(
		not_null<HistoryView::Element*> view,
		std::vector<FullMsgId> &groupIds) const {
	const auto source = target();
	const auto item = view->data();
	if (view->isHidden()
		|| view->isHiddenByGroup()
		|| item->isEmpty()
		|| item->isService()
		|| item->isSponsored()) {
		return false;
	}
	if (source.scheduled) {
		if (!item->isScheduled()
			|| !Data::IsScheduledMsgId(item->id)
			|| item->isLocal()
			|| item->isSending()
			|| item->hasFailed()
			|| source.session().scheduledMessages().lookupItem(item->fullId()) != item.get()) {
			return false;
		}
	} else if (!IsServerMsgId(item->id) || !item->isRegular()) {
		return false;
	}

	const auto main = source.main;
	const auto section = source.section;
	auto total = main ? int(main->_selected.size()) : int(section->_selected.size());
	const auto allowed = [&](not_null<HistoryItem*> candidate) {
		if (_action == Action::Copy) {
			return main
				? !main->hasCopyRestriction(candidate)
				: !section->hasCopyRestriction(candidate);
		}
		return main
			? main->goodForSelection(&main->_selected, candidate, total)
			: section->isGoodForSelection(section->_selected, candidate, total);
	};
	if (const auto group = source.session().data().groups().find(item)) {
		for (const auto other : group->items) {
			if (!allowed(other)) {
				return false;
			}
			groupIds.push_back(other->fullId());
		}
	} else if (!allowed(item)) {
		return false;
	}
	if (_action == Action::Select) {
		return (total <= MaxSelectedItems) && (main
			? !main->isSelectedAsGroup(&main->_selected, item)
			: !section->isSelectedAsGroup(section->_selected, item));
	}

	const auto media = view->media();
	return view->hasVisibleText() || (media && media->hasTextForCopy());
}

std::vector<MessageHints::Entry> MessageHints::snapshot() const {
	auto entries = std::vector<Entry>();
	if (!_view || !_view->isVisible()) {
		return entries;
	}
	const auto select = (_action == Action::Select);
	const auto view = target();
	const auto main = view.main;
	const auto section = view.section;
	if (main) {
		if (main->hasPendingResizedItems()
			|| main->_chooseForReportReason
			|| main->_gestureHorizontal.translation
			|| (select && main->hasSelectRestriction())) {
			return entries;
		}
	} else if (section->_resizePending
		|| section->_refreshingViewer
		|| !section->_itemRevealAnimations.empty()
		|| section->_horizontalScrollLocked
		|| section->_chooseForReportReason
		|| (select && (!section->_selectEnabled || section->hasSelectRestriction()))) {
		return entries;
	}
	const auto controller = main ? main->_controller.get() : section->controllerOrNull();
	const auto visible = clip();
	const auto badgeHeight = LabelSize(u"a"_q).height();
	if (!controller || visible.height() < badgeHeight) {
		return entries;
	}
	const auto collect = [&](
			not_null<HistoryView::Element*> element,
			int top,
			int bottom) {
		auto group = std::vector<FullMsgId>();
		if ((section && element->pendingResize()) || !eligible(element, group)) {
			return true;
		}
		const auto inner = element->innerGeometry();
		const auto topMargin = element->marginTop();
		const auto bottomMargin = element->marginBottom();
		const auto rect = QRect(
			inner.x(),
			top + topMargin,
			inner.width(),
			bottom - top - topMargin - bottomMargin
		).intersected(visible);
		if (rect.height() < badgeHeight
			|| controller->widget()->contentOverlapped(
				QRect(_view->mapToGlobal(rect.topLeft()), rect.size()))) {
			return true;
		}
		entries.push_back({
			.id = element->data()->fullId(),
			.element = element,
			.rect = rect,
			.group = std::move(group),
		});
		return true;
	};
	if (main) {
		main->purpleEnumerateVisible(collect);
	} else {
		section->purpleEnumerateVisible(collect);
	}
	auto digits = 1;
	auto capacity = kHintLetters.size();
	while (entries.size() > capacity) {
		++digits;
		capacity *= kHintLetters.size();
	}
	for (auto i = size_t(0); i != entries.size(); ++i) {
		entries[i].label = HintLabel(i, digits);
	}
	return entries;
}

bool MessageHints::start(Target view, Action action) {
	clear();
	Expects(view);
	_view = view.widget();
	_session = base::make_weak(&view.session());
	_action = action;
	_scheduled = view.scheduled;
	_entries = snapshot();
	if (_entries.empty()) {
		clear();
		return false;
	}
	const auto visible = clip();
	_globalClip = QRect(_view->mapToGlobal(visible.topLeft()), visible.size());
	_overlay = new Overlay(_view.data(), *this);
	_overlay->setGeometry(visible);
	_overlay->show();
	_overlay->raise();
	watch();
	return active();
}

void MessageHints::watch() {
	const auto source = target();
	const auto main = source.main;
	const auto section = source.section;
	const auto &data = source.session().data();
	const auto item = [=, this](const HistoryItem *item) {
		if (!_view) {
			return false;
		} else if (main) {
			return (item->history() == main->_history)
				|| (item->history() == main->_migrated);
		}
		const auto id = item->fullId();
		return section->viewForItem(item) || ranges::any_of(
			_entries,
			[=](const Entry &entry) {
				return entry.id == id || ranges::contains(entry.group, id);
			});
	};
	const auto view = [item](not_null<const HistoryView::Element*> view) {
		return item(view->data());
	};
	auto geometry = main
		? main->_purpleHintGeometryChanges.events()
		: section->_purpleHintGeometryChanges.events();
	const auto scroll = main ? main->_scroll.get() : section->delegate()->listScrollArea();
	rpl::merge(
		std::move(geometry),
		_view->death(),
		_view->geometryValue() | rpl::skip(1) | rpl::to_empty,
		_view->shownValue() | rpl::filter([](bool shown) { return !shown; })
			| rpl::to_empty,
		data.itemRemoved() | rpl::filter(item) | rpl::to_empty,
		data.itemDataChanges() | rpl::filter(item) | rpl::to_empty,
		data.itemResizeRequest() | rpl::filter(item) | rpl::to_empty,
		data.newItemAdded() | rpl::filter(item) | rpl::to_empty,
		data.viewRemoved() | rpl::filter(view) | rpl::to_empty,
		data.viewLayoutChanged() | rpl::filter(view) | rpl::to_empty
	) | rpl::on_next([this] { clear(); }, _watch);
	if (scroll) {
		rpl::merge(
			scroll->geometryValue() | rpl::skip(1) | rpl::to_empty,
			scroll->scrollTopChanges() | rpl::to_empty
		) | rpl::on_next([this] { clear(); }, _watch);
	}
	if (main) {
		rpl::merge(data.historyUnloaded(), data.historyCleared()
		) | rpl::filter([=, this](const History *history) {
			return _view && ((history == main->_history)
				|| (history == main->_migrated));
		}) | rpl::on_next([this] { clear(); }, _watch);
	}
}

MessageHints::Input MessageHints::type(int key, Target view) {
	if (!active() || !view || view.widget() != _view.data()
		|| view.scheduled != _scheduled || &view.session() != _session.get()) {
		clear();
		return Input::Finished;
	}
	const auto visible = clip();
	if (_globalClip != QRect(_view->mapToGlobal(visible.topLeft()),
			visible.size()) || snapshot() != _entries) {
		clear();
		return Input::Finished;
	}
	if (key < Qt::Key_A || key > Qt::Key_Z) {
		clear();
		return Input::Unknown;
	}
	_prefix.append(QChar(key - Qt::Key_A + 'a'));
	const auto i = ranges::find_if(_entries, [&](const Entry &entry) {
		return entry.label.startsWith(_prefix);
	});
	if (i == end(_entries)) {
		clear();
		return Input::Unknown;
	} else if (i->label != _prefix) {
		_overlay->update();
		return Input::Pending;
	}
	const auto item = _session->data().message(i->id);
	const auto valid = item && (view.main
		? view.main->viewByItem(item)
		: view.section->viewForItem(item)) == i->element;
	const auto action = _action;
	clear();
	if (valid) {
		apply(view, item, action);
	}
	return Input::Finished;
}

void MessageHints::apply(
		Target view,
		not_null<HistoryItem*> item,
		Action action) {
	if (const auto main = view.main) {
		if (action == Action::Copy) {
			main->copyContextText(item->fullId());
			return;
		}
		main->clearTextSelection();
		main->changeSelectionAsGroup(
			&main->_selected, item, HistoryInner::SelectAction::Select);
		main->_accessibilitySelectionAnchor = nullptr;
		main->repaintItem(item);
		main->_widget->updateTopBarSelection(true);
	} else if (action == Action::Copy) {
		if (view.section->showCopyRestriction(item)) {
			return;
		}
		auto &session = view.section->session();
		if (const auto group = session.data().groups().find(item)) {
			TextUtilities::SetClipboardText(HistoryGroupText(group));
		} else {
			Iv::SetRichBlocksClipboard(
				HistoryItemText(item),
				HistoryItemRichBlocks(item),
				&session);
		}
	} else {
		const auto section = QPointer(view.section);
		const auto previous = std::exchange(
			section->_purpleKeepSelectionFocus, true);
		const auto restore = gsl::finally([=] {
			if (section) {
				section->_purpleKeepSelectionFocus = previous;
			}
		});
		section->selectItemAsGroup(item);
	}
}

void MessageHints::clear() {
	_watch.destroy();
	delete _overlay.data();
	_view = nullptr;
	_session = {};
	_entries.clear();
	_prefix.clear();
	_globalClip = {};
}

} // namespace Purple

#include "purple/purple_message_hints.h"

#include "config.h"
#include "data/data_groups.h"
#include "data/data_session.h"
#include "history/history_inner_widget.h"
#include "history/history_item.h"
#include "history/history_widget.h"
#include "history/view/history_view_element.h"
#include "history/view/media/history_view_media.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "ui/round_rect.h"
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
	Overlay(not_null<HistoryInner*> parent, const MessageHints &hints)
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

QRect MessageHints::clip() const {
	return QRect(
		0,
		_view->_visibleAreaTop,
		_view->width(),
		_view->_visibleAreaBottom - _view->_visibleAreaTop
	).intersected(_view->rect());
}

bool MessageHints::eligible(
		not_null<HistoryView::Element*> view,
		std::vector<FullMsgId> &groupIds) const {
	const auto item = view->data();
	if (view->isHidden()
		|| view->isHiddenByGroup()
		|| !IsServerMsgId(item->id)
		|| !item->isRegular()
		|| item->isEmpty()
		|| item->isService()
		|| item->isSponsored()) {
		return false;
	}

	auto total = int(_view->_selected.size());
	const auto allowed = [&](not_null<HistoryItem*> candidate) {
		return (_action == Action::Select)
			? _view->goodForSelection(&_view->_selected, candidate, total)
			: !_view->hasCopyRestriction(candidate);
	};
	if (const auto group = _view->session().data().groups().find(item)) {
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
		return (total <= MaxSelectedItems)
			&& !_view->isSelectedAsGroup(&_view->_selected, item);
	}

	const auto media = view->media();
	return view->hasVisibleText() || (media && media->hasTextForCopy());
}

std::vector<MessageHints::Entry> MessageHints::snapshot() const {
	auto entries = std::vector<Entry>();
	if (!_view
		|| !_view->isVisible()
		|| _view->hasPendingResizedItems()
		|| _view->_chooseForReportReason
		|| _view->_gestureHorizontal.translation
		|| (_action == Action::Select && _view->hasSelectRestriction())) {
		return entries;
	}
	const auto visible = clip();
	const auto badgeHeight = LabelSize(u"a"_q).height();
	if (visible.height() < badgeHeight) {
		return entries;
	}
	_view->purpleEnumerateVisible([&](
			not_null<HistoryView::Element*> element,
			int top,
			int bottom) {
		auto group = std::vector<FullMsgId>();
		if (!eligible(element, group)) {
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
			|| _view->_controller->widget()->contentOverlapped(
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
	});
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

bool MessageHints::start(not_null<HistoryInner*> view, Action action) {
	clear();
	_view = view.get();
	_session = base::make_weak(&view->session());
	_action = action;
	_entries = snapshot();
	if (_entries.empty()) {
		clear();
		return false;
	}
	const auto visible = clip();
	_globalClip = QRect(view->mapToGlobal(visible.topLeft()), visible.size());
	_overlay = new Overlay(view, *this);
	_overlay->setGeometry(visible);
	_overlay->show();
	_overlay->raise();
	watch();
	return true;
}

void MessageHints::watch() {
	const auto &data = _view->session().data();
	const auto item = [this](const HistoryItem *item) {
		return _view && (item->history() == _view->_history
			|| item->history() == _view->_migrated);
	};
	const auto view = [item](not_null<const HistoryView::Element*> view) {
		return item(view->data());
	};
	const auto history = [this](const History *history) {
		return _view && (history == _view->_history
			|| history == _view->_migrated);
	};
	rpl::merge(
		_view->_purpleHintGeometryChanges.events(),
		_view->geometryValue() | rpl::skip(1) | rpl::to_empty,
		_view->shownValue() | rpl::filter([](bool shown) { return !shown; })
			| rpl::to_empty,
		_view->_scroll->geometryValue() | rpl::skip(1) | rpl::to_empty,
		_view->_scroll->scrollTopChanges() | rpl::to_empty,
		data.itemRemoved() | rpl::filter(item) | rpl::to_empty,
		data.itemDataChanges() | rpl::filter(item) | rpl::to_empty,
		data.itemResizeRequest() | rpl::filter(item) | rpl::to_empty,
		data.newItemAdded() | rpl::filter(item) | rpl::to_empty,
		data.viewRemoved() | rpl::filter(view) | rpl::to_empty,
		data.viewLayoutChanged() | rpl::filter(view) | rpl::to_empty,
		data.historyUnloaded() | rpl::filter(history) | rpl::to_empty,
		data.historyCleared() | rpl::filter(history) | rpl::to_empty
	) | rpl::on_next([this] { clear(); }, _watch);
}

MessageHints::Input MessageHints::type(int key, HistoryInner *view) {
	if (!active() || !view || view != _view || &_view->session() != _session.get()) {
		clear();
		return Input::Finished;
	}
	const auto visible = clip();
	if (_globalClip != QRect(view->mapToGlobal(visible.topLeft()),
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
	const auto valid = item && view->viewByItem(item) == i->element;
	const auto action = _action;
	clear();
	if (valid) {
		apply(view, item, action);
	}
	return Input::Finished;
}

void MessageHints::apply(
		not_null<HistoryInner*> view,
		not_null<HistoryItem*> item,
		Action action) {
	if (action == Action::Copy) {
		view->copyContextText(item->fullId());
		return;
	}
	view->clearTextSelection();
	view->changeSelectionAsGroup(
		&view->_selected, item, HistoryInner::SelectAction::Select);
	view->_accessibilitySelectionAnchor = nullptr;
	view->repaintItem(item);
	view->_widget->updateTopBarSelection(true);
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

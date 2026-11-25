#include "purple/purple_message_hints.h"
#include "purple/purple_message_hint_placement.h"

#include "base/unixtime.h"
#include "config.h"
#include "data/components/scheduled_messages.h"
#include "data/data_channel.h"
#include "data/data_groups.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_inner_widget.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "history/history_item_text.h"
#include "history/history_widget.h"
#include "history/view/history_view_context_menu.h"
#include "history/view/history_view_element.h"
#include "history/view/history_view_list_widget.h"
#include "history/view/history_view_message.h"
#include "history/view/media/history_view_media.h"
#include "iv/iv_rich_message_html_export.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "ui/basic_click_handlers.h"
#include "ui/painter.h"
#include "ui/round_rect.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/elastic_scroll.h"
#include "window/window_session_controller.h"

#include "styles/palette.h"
#include "styles/style_chat.h"
#include "styles/style_widgets.h"

#include <QtGui/QContextMenuEvent>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QPainter>

#include <array>
#include <string_view>

namespace Purple {
namespace {

class MessageHintMenuEvent final : public QContextMenuEvent {
public:
	MessageHintMenuEvent(
		not_null<HistoryItem*> item,
		QPoint local,
		QPoint global)
	: QContextMenuEvent(QContextMenuEvent::Keyboard, local, global)
	, item(item) {
	}

	const not_null<HistoryItem*> item;

};

bool CanCopyMessageLink(not_null<HistoryItem*> item) {
	return IsServerMsgId(item->id)
		&& item->hasDirectLink()
		&& !IsAnchoredEphemeral(item);
}

HistoryView::TextState BodyLinkState(
		not_null<const HistoryView::Element*> element,
		QPoint point) {
	return element->textState(point, { .onlyMessageText = true });
}

uint16 RenderedOffset(const Ui::Text::String &text, int offset) {
	// Exporting can expand an inline emoji beyond its rendered text length.
	auto left = 0;
	auto right = text.length();
	while (left < right) {
		const auto middle = left + (right - left) / 2;
		if (text.toString({ 0, uint16(middle) }).size() < offset) {
			left = middle + 1;
		} else {
			right = middle;
		}
	}
	return uint16(left);
}

bool IsUrl(EntityType type) {
	return type == EntityType::Url || type == EntityType::CustomUrl;
}

struct BodyLinkHit {
	QRect rect;
	QString url;
};

void CollectVisibleBodyLinks(
		not_null<HistoryView::Message*> message,
		const Ui::Text::String &text,
		const Ui::Text::HighlightRectsRequest &geometry,
		QPoint origin,
		std::vector<BodyLinkHit> &result) {
	const auto exported = text.toTextWithEntities();
	for (const auto &entity : exported.entities) {
		if (!IsUrl(entity.type())) {
			continue;
		}
		const auto range = TextSelection{
			RenderedOffset(text, entity.offset()),
			RenderedOffset(text, entity.offset() + entity.length()),
		};
		if (range.empty()) {
			continue;
		}
		const auto ranges = std::array{ range };
		const auto rects = text.countHighlightRects(ranges, geometry);
		for (const auto &part : rects) {
			const auto rect = part.intersected(geometry.clip);
			if (rect.isEmpty()) {
				continue;
			}
			const auto point = rect.center();
			auto request = Ui::Text::StateRequest();
			request.align = geometry.align;
			const auto leafState = text.getState(
				point - geometry.position,
				geometry.geometry,
				request);
			if (!leafState.link || text.linkRangeFor(leafState.link) != range) {
				continue;
			}
			const auto state = BodyLinkState(message, point + origin);
			if (state.link && IsUrl(state.link->getTextEntity().type)
				&& state.link->url() == leafState.link->url()
				&& !state.link->url().isEmpty()
				&& state.itemId == message->textItem()->fullId()) {
				result.push_back({ rect.translated(origin), state.link->url() });
				break;
			}
		}
	}
}

std::vector<BodyLinkHit> VisibleBodyLinks(
		not_null<HistoryView::Message*> message,
		QRect clip) {
	auto result = std::vector<BodyLinkHit>();
	const auto [article, origin] = message->purpleRichLinkLayout();
	const auto [text, bounds] = message->purpleBodyLinkLayout();
	if (article) {
		article->forEachVisibleLinkText(clip.translated(-origin), [&](
				const Ui::Text::String &leaf,
				const Ui::Text::HighlightRectsRequest &geometry) {
			CollectVisibleBodyLinks(message, leaf, geometry, origin, result);
		});
	} else if (text && clip.intersects(bounds)) {
		CollectVisibleBodyLinks(message, *text, {
			.position = bounds.topLeft(),
			.geometry = Ui::Text::SimpleGeometry(bounds.width(), 0, 0, false),
			.clip = clip.intersected(bounds),
		}, QPoint(), result);
	}
	return result;
}

QRect PlaceLinkBadge(
		QRect box,
		QSize space,
		const std::vector<QRect> &placed) {
	const auto overlaps = [&](const QRect &rect) {
		return ranges::any_of(placed, [&](const QRect &other) {
			return rect.intersects(other);
		});
	};
	if (!overlaps(box)) {
		return box;
	}
	for (auto dy = box.height(); dy < space.height(); dy += box.height()) {
		for (const auto offset : { dy, -dy }) {
			const auto other = box.translated(0, offset);
			if (other.top() >= 0 && other.bottom() < space.height()
				&& !overlaps(other)) {
				return other;
			}
		}
	}
	return box;
}

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

HistoryItem *MessageHintMenuItem(
		QContextMenuEvent *event,
		HistoryItem *focused) {
	const auto hint = dynamic_cast<const MessageHintMenuEvent*>(event);
	return hint ? hint->item.get() : focused;
}

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
		auto badges = std::vector<QRect>();
		for (const auto &entry : _hints._entries) {
			const auto matches = entry.label.startsWith(_hints._prefix);
			if (!matches && !entry.bodyLink) {
				continue;
			}
			const auto anchor = entry.rect.translated(-geometry().topLeft());
			const auto bubble = entry.bubbleRect.translated(-geometry().topLeft());
			const auto box = MessageHintBadgeRect(
				bubble, anchor.y(), LabelSize(entry.label), rect(),
				st::lineWidth, style::RightToLeft());
			const auto placed = entry.bodyLink
				? PlaceLinkBadge(box, this->size(), badges)
				: box;
			if (entry.bodyLink) {
				badges.push_back(placed);
			}
			if (matches) {
				if (placed != box && !placed.intersects(anchor)) {
					p.setPen(QPen(QColor(0xb8, 0x92, 0x00), 1.5));
					p.drawLine(placed.center(), MessageHintLeaderEnd(placed.center(), anchor));
				}
				auto hq = PainterHighQualityEnabler(p);
				p.setPen(QPen(QColor(0xb8, 0x92, 0x00), 1));
				p.setBrush(QColor(0xff, 0xd8, 0x4a));
				p.drawRoundedRect(QRectF(placed).adjusted(.5, .5, -.5, -.5), 4, 4);
				const auto upper = entry.label.toUpper();
				const auto typedLen = int(_hints._prefix.size());
				if (typedLen > 0 && typedLen < upper.size()) {
					const auto fm = QFontMetrics(p.font());
					const auto typed = upper.left(typedLen);
					const auto rest = upper.mid(typedLen);
					const auto totalW = fm.horizontalAdvance(upper);
					const auto typedW = fm.horizontalAdvance(typed);
					const auto startX = placed.x() + (placed.width() - totalW) / 2;
					p.setPen(QColor(0xb9, 0x1c, 0x1c));
					p.drawText(QRect(startX, placed.y(), typedW, placed.height()),
						Qt::AlignLeft | Qt::AlignVCenter,
						typed);
					p.setPen(QColor(0x1c, 0x19, 0x17));
					p.drawText(QRect(startX + typedW, placed.y(), totalW - typedW, placed.height()),
						Qt::AlignLeft | Qt::AlignVCenter,
						rest);
				} else {
					p.setPen(QColor(0x1c, 0x19, 0x17));
					p.drawText(placed, Qt::AlignCenter, upper);
				}
			}
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

bool MessageHints::Target::supportsCompose(Action action) const {
	if (main) {
		return true;
	} else if (!section || (scheduled && action == Action::Reply)) {
		return false;
	}
	using Context = HistoryView::Context;
	const auto context = section->elementContext();
	return (context == Context::History)
		|| (context == Context::Replies)
		|| (context == Context::Monoforum)
		|| (context == Context::ScheduledTopic && action == Action::Edit);
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

bool MessageHints::canCompose(
		Target view,
		not_null<HistoryItem*> item,
		Action action) {
	if (!view.supportsCompose(action)
		|| view.session().frozen()
		|| IsAnchoredEphemeral(item)) {
		return false;
	} else if (action == Action::Reply) {
		return CanSendReply(item) || item->allowsForward();
	} else if ((view.main
			? view.main->hasSelectedItems()
			: view.section->hasSelectedItems())
		|| item->isSending()
		|| item->hasFailed()
		|| !item->allowsEdit(base::unixtime::now())) {
		return false;
	}
	const auto channel = item->history()->peer->asChannel();
	return view.main || !channel || channel->isMegagroup()
		|| channel->canEditMessages();
}

bool MessageHints::canUseBodyLink(
		Target view,
		not_null<HistoryItem*> item,
		Action action) {
	if (view.session().frozen() || IsAnchoredEphemeral(item)) {
		return false;
	}
	return (action != Action::CopyUrl) || (view.main
		? !view.main->hasCopyRestriction(item)
		: !view.section->hasCopyRestriction(item));
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

	if (_action == Action::Link) {
		return !source.scheduled && CanCopyMessageLink(item);
	} else if (_action == Action::CopyUrl || _action == Action::OpenUrl) {
		const auto owner = view->textItem();
		return owner && canUseBodyLink(source, owner, _action);
	} else if (_action == Action::Reply || _action == Action::Edit) {
		return canCompose(source, item, _action);
	}

	const auto main = source.main;
	const auto section = source.section;
	auto total = main ? int(main->_selected.size()) : int(section->_selected.size());
	const auto allowed = [&](not_null<HistoryItem*> candidate) {
		if (_action == Action::Menu) {
			return true;
		} else if (_action == Action::Copy) {
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
	return _action == Action::Menu
		|| view->hasVisibleText()
		|| (media && media->hasTextForCopy());
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
		if (_action == Action::CopyUrl || _action == Action::OpenUrl) {
			const auto message = dynamic_cast<HistoryView::Message*>(element.get());
			if (message) {
				const auto owner = message->textItem()->fullId();
				auto hits = VisibleBodyLinks(message, rect.translated(0, -top));
				for (auto &hit : hits) {
					const auto glyph = hit.rect.translated(0, top);
					entries.push_back({
						.id = element->data()->fullId(),
						.element = element,
						.rect = glyph,
						.bubbleRect = rect,
						.bodyLink = BodyLink{
							.owner = owner,
							.itemPoint = hit.rect.center(),
							.listPoint = glyph.center(),
							.url = std::move(hit.url),
						},
					});
				}
			}
			return true;
		}
		entries.push_back({
			.id = element->data()->fullId(),
			.element = element,
			.rect = rect,
			.bubbleRect = rect,
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
				return entry.id == id || ranges::contains(entry.group, id)
					|| (entry.bodyLink && entry.bodyLink->owner == id);
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
	const auto widget = QPointer(view.widget());
	const auto id = i->id;
	const auto element = i->element;
	const auto point = i->rect.center();
	const auto bodyLink = i->bodyLink;
	const auto action = _action;
	clear();
	if (!widget) {
		return Input::Finished;
	}
	const auto item = view.session().data().message(id);
	if (item && (view.main
		? view.main->viewByItem(item)
		: view.section->viewForItem(item)) == element) {
		return bodyLink
			? applyBodyLink(view, element, action, *bodyLink)
			: apply(view, item, action, point);
	}
	return Input::Finished;
}

MessageHints::Input MessageHints::applyBodyLink(
		Target view,
		not_null<const HistoryView::Element*> element,
		Action action,
		const BodyLink &link) {
	const auto widget = view.widget();
	const auto owner = view.session().data().message(link.owner);
	if (!owner || element->textItem() != owner
		|| !widget->visibleRegion().contains(link.listPoint)
		|| !canUseBodyLink(view, owner, action)) {
		return Input::Finished;
	}
	const auto state = BodyLinkState(element, link.itemPoint);
	if (state.itemId != link.owner || !state.link
		|| !IsUrl(state.link->getTextEntity().type)
		|| state.link->url() != link.url) {
		return Input::Finished;
	}
	if (action == Action::CopyUrl) {
		const auto text = state.link->copyToClipboardText();
		if (!text.isEmpty()) {
			QGuiApplication::clipboard()->setText(text);
		}
		return Input::Finished;
	}
	ActivateClickHandler(widget, state.link, view.main
		? view.main->prepareClickContext(Qt::LeftButton, link.owner)
		: view.section->prepareClickContext(Qt::LeftButton, link.owner));
	return Input::LeaveNormal;
}

MessageHints::Input MessageHints::apply(
		Target view,
		not_null<HistoryItem*> item,
		Action action,
		QPoint point) {
	if (action == Action::Reply || action == Action::Edit) {
		if (!canCompose(view, item, action)) {
			return Input::Finished;
		}
		const auto id = item->fullId();
		if (action == Action::Reply) {
			if (const auto main = view.main) {
				main->elementReplyTo({ .messageId = id });
			} else {
				view.section->replyToMessageRequestNotify({ .messageId = id });
			}
		} else if (const auto main = view.main) {
			const auto selection = main->getSelectedTextRange(item);
			if (!selection.empty()) {
				main->clearSelected(true);
			}
			main->_widget->editMessage(item, selection);
		} else {
			view.section->editMessageRequestNotify(id);
		}
		return Input::LeaveNormal;
	}
	if (action == Action::Link) {
		if (view.scheduled || !CanCopyMessageLink(item)) {
			return Input::Finished;
		}
		const auto controller = view.main
			? view.main->_widget->controller()
			: view.section->controller();
		if (&controller->session() != &view.session()) {
			return Input::Finished;
		}
		const auto context = view.main
			? HistoryView::Context::History
			: view.section->elementContext();
		HistoryView::CopyPostLink(controller, item->fullId(), context);
		return Input::Finished;
	}
	if (action == Action::Menu) {
		const auto widget = view.widget();
		if (!widget->isVisible() || !widget->visibleRegion().contains(point)) {
			return Input::Finished;
		}
		auto event = MessageHintMenuEvent(item, point, widget->mapToGlobal(point));
		if (const auto main = view.main) {
			main->showContextMenu(&event);
		} else {
			view.section->showContextMenu(&event);
		}
		return Input::LeaveNormal;
	}
	if (const auto main = view.main) {
		if (action == Action::Copy) {
			main->copyContextText(item->fullId());
			return Input::Finished;
		}
		main->clearTextSelection();
		main->changeSelectionAsGroup(
			&main->_selected, item, HistoryInner::SelectAction::Select);
		main->_accessibilitySelectionAnchor = nullptr;
		main->repaintItem(item);
		main->_widget->updateTopBarSelection(true);
	} else if (action == Action::Copy) {
		if (view.section->showCopyRestriction(item)) {
			return Input::Finished;
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
	return Input::Finished;
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

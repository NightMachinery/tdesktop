#include "purple/purple_message_hints.h"
#include "purple/purple_hint_labels.h"
#include "purple/purple_hint_paint.h"
#include "purple/purple_message_hint_placement.h"

#include "base/unixtime.h"
#include "config.h"
#include "data/components/scheduled_messages.h"
#include "data/data_channel.h"
#include "data/data_groups.h"
#include "data/data_message_reactions.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_inner_widget.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "history/history_item_text.h"
#include "history/history_widget.h"
#include "history/view/history_view_context_menu.h"
#include "history/view/history_view_cursor_state.h"
#include "history/view/history_view_element.h"
#include "history/view/history_view_list_widget.h"
#include "history/view/history_view_message.h"
#include "history/view/media/history_view_media.h"
#include "iv/iv_rich_message_html_export.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "ui/basic_click_handlers.h"
#include "ui/painter.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/elastic_scroll.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"

#include "styles/palette.h"
#include "styles/style_chat.h"
#include "styles/style_widgets.h"

#include <QtGui/QContextMenuEvent>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QPainter>
#include <QtWidgets/QApplication>

#include <array>

namespace Purple {
namespace {

class MessageHintMenuEvent final : public QContextMenuEvent {
public:
	MessageHintMenuEvent(
		not_null<HistoryItem*> item,
		QPoint local,
		QPoint global,
		bool capture = false)
	: QContextMenuEvent(QContextMenuEvent::Keyboard, local, global)
	, item(item)
	, capture(capture) {
	}

	const not_null<HistoryItem*> item;
	const bool capture;
	WindowHintTarget::Menu menu;

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

bool MatchesUrl(const ClickHandlerPtr &handler, const QString &url) {
	return handler && !url.isEmpty()
		&& IsUrl(handler->getTextEntity().type)
		&& handler->url() == url;
}

struct BodyLinkHit {
	QRect rect;
	ClickHandlerPtr handler;
};

void CollectVisibleBodyLinks(
		not_null<HistoryView::Message*> message,
		const Ui::Text::String &text,
		const Ui::Text::HighlightRectsRequest &geometry,
		QPoint origin,
		bool all,
		std::vector<BodyLinkHit> &result) {
	const auto exported = text.toTextWithEntities();
	for (const auto &entity : exported.entities) {
		if (!all && !IsUrl(entity.type())) {
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
			if (!state.link || state.itemId != message->textItem()->fullId()) {
				continue;
			}
			if ((all && state.link == leafState.link)
				|| MatchesUrl(state.link, leafState.link->url())) {
				result.push_back({ rect.translated(origin), state.link });
				break;
			}
		}
	}
}

std::vector<BodyLinkHit> VisibleBodyLinks(
		not_null<HistoryView::Message*> message,
		QRect clip,
		bool all = false) {
	auto result = std::vector<BodyLinkHit>();
	const auto [article, origin] = message->purpleRichLinkLayout();
	const auto [text, bounds] = message->purpleBodyLinkLayout();
	if (article) {
		article->forEachVisibleLinkText(clip.translated(-origin), [&](
				const Ui::Text::String &leaf,
				const Ui::Text::HighlightRectsRequest &geometry) {
			CollectVisibleBodyLinks(message, leaf, geometry, origin, all, result);
		});
	} else if (text && clip.intersects(bounds)) {
		CollectVisibleBodyLinks(message, *text, {
			.position = bounds.topLeft(),
			.geometry = Ui::Text::SimpleGeometry(bounds.width(), 0, 0, false),
			.clip = clip.intersected(bounds),
		}, QPoint(), all, result);
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

bool CapturingHintMenu(QContextMenuEvent *event) {
	const auto hint = dynamic_cast<const MessageHintMenuEvent*>(event);
	return hint && hint->capture;
}

bool CaptureHintMenu(QContextMenuEvent *event, WindowHintTarget::Menu &menu) {
	if (CapturingHintMenu(event)) {
		static_cast<MessageHintMenuEvent*>(event)->menu = std::move(menu);
		return true;
	}
	return false;
}

class MessageHints::Overlay final : public Ui::RpWidget {
public:
	Overlay(not_null<Ui::RpWidget*> parent, const MessageHints &hints)
	: Ui::RpWidget(parent)
	, _hints(hints) {
		setFocusPolicy(Qt::NoFocus);
		setAttribute(Qt::WA_TransparentForMouseEvents);
	}

protected:
	void paintEvent(QPaintEvent *) override {
		auto p = QPainter(this);
		p.setFont(st::defaultToast.style.font);
		p.translate(-geometry().topLeft());
		p.setRenderHint(QPainter::Antialiasing);
		const auto rtl = style::RightToLeft();
		for (const auto &entry : _hints._entries) {
			if (!entry.label.startsWith(_hints._prefix)) continue;
			const auto &colors = _hints._palette.colors[int(entry.role)];
			const auto line = entry.placement.leader;
			if (!line.isNull()) {
				p.setPen(QPen(st::windowBg->c, 3 * st::lineWidth));
				p.drawLine(line);
				p.setPen(QPen(colors.edge, st::lineWidth));
				p.drawLine(line);
				p.setBrush(colors.edge);
				p.drawEllipse(line.p2(), st::lineWidth, st::lineWidth);
			}
			if (!_hints._prefix.isEmpty()) {
				p.setPen(QPen(colors.edge, 2 * st::lineWidth));
				p.setBrush(Qt::NoBrush);
				p.drawRect(entry.rect);
			}
		}
		for (const auto &entry : _hints._entries) {
			if (entry.label.startsWith(_hints._prefix)) {
				PaintHintBadge(p, entry.placement.badge, entry.label.toUpper(),
					_hints._prefix.size(), entry.role, _hints._palette, rtl);
			}
		}
	}

private:
	const MessageHints &_hints;
};

MessageHints::MessageHints() = default;

MessageHints::~MessageHints() {
	clear();
}

bool MessageHints::active() const {
	return _overlay != nullptr;
}

bool MessageHints::windowActive() const {
	return active() && _windowRoot;
}

bool MessageHints::affectedBy(QWidget *widget) const {
	return windowActive() && widget != _overlay.data()
		&& (widget == _windowRoot || _windowRoot->isAncestorOf(widget)
			|| widget->isAncestorOf(_windowRoot));
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
				const auto hits = VisibleBodyLinks(message, rect.translated(0, -top));
				for (const auto &hit : hits) {
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
							.url = hit.handler->url(),
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
	for (auto &entry : entries) {
		entry.role = _action == Action::Reply ? HintRole::Reply
			: _action == Action::Menu ? HintRole::Menu
			: _action == Action::OpenUrl ? HintRole::Navigate : HintRole::General;
	}
	layout(entries, visible);
	std::erase_if(entries, [](const Entry &entry) { return entry.placement.badge.isEmpty(); });
	return entries;
}

void MessageHints::layout(std::vector<Entry> &entries, QRect viewport) const {
	auto regions = std::vector<HintGeometry>();
	regions.reserve(entries.size());
	const auto labels = HintLabels(entries.size());
	for (auto i = size_t(0); i != entries.size(); ++i) {
		auto &entry = entries[i];
		entry.label = QString::fromStdString(labels[i]);
		auto size = LabelSize(entry.label.toUpper());
		size.rwidth() += _palette.inset(entry.role);
		const auto compactWidth = size.width() - st::msgServicePadding.left()
			- st::msgServicePadding.right() + 2 * st::lineWidth;
		regions.push_back({ entry.rect, size, entry.row,
			!entry.bubbleRect.isEmpty() && !entry.bodyLink, compactWidth });
	}
	const auto placed = LayoutHintBadges(regions, viewport, st::lineWidth, style::RightToLeft());
	for (auto i = size_t(0); i != entries.size(); ++i) {
		entries[i].placement = placed[i];
	}
}

bool MessageHints::start(Target view, Action action) {
	clear();
	Expects(view);
	_view = view.widget();
	_session = base::make_weak(&view.session());
	_action = action;
	_scheduled = view.scheduled;
	_palette = CurrentHintPalette();
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

bool MessageHints::startWindow(
		Ui::RpWidget *root,
		Main::Session *session,
		QWidget *focus,
		bool menus) {
	clear();
	if (!root || !session || session->frozen()) {
		return false;
	}
	_windowRoot = root;
	_windowMenus = menus;
	_session = base::make_weak(session);
	auto targets = CollectWindowHintTargets(root);
	for (const auto widget : root->findChildren<QWidget*>()) {
		const auto main = dynamic_cast<HistoryInner*>(widget);
		const auto section = dynamic_cast<HistoryView::ListWidget*>(widget);
		if (main || section) {
			appendWindowMessages({ .main = main, .section = section }, session, menus, targets);
		}
	}
	const auto focusCenter = focus ? focus->mapToGlobal(focus->rect().center())
		: root->mapToGlobal(root->rect().center());
	const auto rank = [&](const WindowHintTarget &entry) {
		return std::pair(!entry.mainList, (entry.bounds.center() - focusCenter).manhattanLength());
	};
	std::stable_sort(begin(targets), end(targets), [&](const auto &a, const auto &b) {
		return rank(a) < rank(b);
	});
	const auto origin = root->mapToGlobal(QPoint());
	const auto viewport = WindowHintClip(root).translated(-origin);
	_palette = CurrentHintPalette();
	for (auto &entry : targets) {
		if (!WindowHintExposed(entry.widget, entry.bounds.center())) continue;
		_entries.push_back({
			.rect = entry.bounds.translated(-origin),
			.bubbleRect = entry.bubble.isEmpty() ? QRect() : entry.bubble.translated(-origin),
			.role = menus ? HintRole::Menu : entry.role,
			.row = entry.mainList,
		});
		_windowEntries.push_back(std::move(entry));
	}
	layout(_entries, viewport);
	auto kept = size_t(0);
	for (auto i = size_t(0); i != _entries.size(); ++i) {
		if (_entries[i].placement.badge.isEmpty()) continue;
		if (kept != i) {
			_entries[kept] = std::move(_entries[i]);
			_windowEntries[kept] = std::move(_windowEntries[i]);
		}
		++kept;
	}
	_entries.resize(kept);
	_windowEntries.resize(kept);
	if (_entries.empty()) {
		clear();
		return false;
	}
	_globalClip = WindowHintClip(root);
	_overlay = new Overlay(root, *this);
	_overlay->setGeometry(root->rect());
	_overlay->show();
	_overlay->raise();
	const auto &data = session->data();
	rpl::merge(
		data.newItemAdded() | rpl::to_empty,
		data.itemRemoved() | rpl::to_empty,
		data.itemDataChanges() | rpl::to_empty,
		data.viewLayoutChanged() | rpl::to_empty
	) | rpl::on_next([this] { clear(); }, _watch);
	return true;
}

void MessageHints::appendWindowMessages(
		Target view,
		Main::Session *session,
		bool menus,
		std::vector<WindowHintTarget> &out) {
	const auto widget = view.widget();
	const auto clip = WindowHintClip(widget);
	if (clip.isEmpty() || &view.session() != session
		|| (view.main && (view.main->hasPendingResizedItems() || view.main->_chooseForReportReason))
		|| (view.section && (view.section->_resizePending || view.section->_refreshingViewer
			|| view.section->_chooseForReportReason))) {
		return;
	}
	view.scheduled = view.section && view.section->elementContext() == HistoryView::Context::ScheduledTopic;
	const auto origin = widget->mapToGlobal(QPoint());
	const auto viewport = clip.translated(-origin);
	const auto guard = QPointer(widget);
	const auto alive = base::make_weak(session);
	const auto collect = [&](not_null<HistoryView::Element*> element, int top, int bottom) {
		const auto item = element->data();
		if (element->isHidden() || element->isHiddenByGroup()
			|| item->history()->purpleHiddenFromView()) {
			return true;
		}
		const auto id = item->fullId();
		const auto delta = QPoint(0, top);
		const auto inner = element->innerGeometry();
		const auto bubble = QRect(inner.x(), top + element->marginTop(), inner.width(),
			bottom - top - element->marginTop() - element->marginBottom());
		const auto current = [=] {
			const auto item = alive ? alive->data().message(id) : nullptr;
			return guard && item && !item->history()->purpleHiddenFromView() && (view.main
				? view.main->viewByItem(item) : view.section->viewForItem(item)) == element
				&& (view.main ? view.main->itemTop(element) : view.section->itemTop(element)) == top;
		};
		const auto clippedBubble = bubble.intersected(viewport);
		const auto message = !clippedBubble.isEmpty() && !item->isEmpty() && !item->isService();
		const auto reply = message && item->isRegular() && !item->isSponsored()
			&& IsServerMsgId(item->id) && CanSendReply(item) && canCompose(view, item, Action::Reply);
		if (reply || (menus && message)) {
			const auto reactions = menus ? Data::LookupPossibleReactions(item) : Data::PossibleItemReactionsRef();
			auto messages = std::vector<FullMsgId>{ id };
			if (const auto group = session->data().groups().find(item)) {
				for (const auto part : group->items) {
					messages.push_back(part->fullId());
				}
			}
			out.push_back({
				.widget = widget,
				.bounds = clippedBubble.translated(origin),
				.bubble = bubble.translated(origin),
				.name = u"Message actions"_q,
				.reply = reply,
				.kind = WindowHintTarget::Kind::Message,
				.role = reply ? HintRole::Reply : HintRole::Menu,
				.messages = std::move(messages),
				.current = [=] { return current() && (menus || CanSendReply(element->data())); },
				.activate = reply ? Fn<void()>([=] {
					apply(view, element->data(), Action::Reply, clippedBubble.center());
				}) : Fn<void()>(),
				.nativeMenu = [=] { return buildMenu(view, element, clippedBubble.center()); },
				.fullMenu = [=] { apply(view, element->data(), Action::Menu, clippedBubble.center()); },
				.canReact = !reactions.recent.empty() || !reactions.stickers.empty() || reactions.customAllowed,
			});
		}
		auto handlers = QSet<const ClickHandler*>();
		const auto add = [&](QRect bounds, bool body, ClickHandlerPtr handler, FullMsgId owner) {
			if (!handler || handlers.contains(handler.get())) {
				return;
			}
			handlers.insert(handler.get());
			const auto point = bounds.center();
			const auto url = body ? handler->url() : QString();
			const auto hit = [=] {
				const auto state = body
					? BodyLinkState(element, point)
					: element->textState(point, {});
				if (state.link == handler
					|| (state.itemId == owner && MatchesUrl(state.link, url))) {
					return state.link;
				}
				return ClickHandlerPtr();
			};
			out.push_back({
				.widget = widget,
				.bounds = bounds.translated(origin + delta),
				.name = handler->tooltip(),
				.role = WindowHintHandlerRole(handler),
				.current = [=] {
					return current() && widget->mapToGlobal(QPoint()) == origin
						&& bool(hit());
				},
				.activate = [=] {
					if (const auto link = hit()) {
						ActivateClickHandler(widget, link, view.main
							? view.main->prepareClickContext(Qt::LeftButton, owner)
							: view.section->prepareClickContext(Qt::LeftButton, owner));
					}
				},
			});
		};
		const auto region = QRect(0, top, widget->width(), bottom - top).intersected(viewport)
			.translated(-delta);
		if (const auto message = dynamic_cast<HistoryView::Message*>(element.get())) {
			for (const auto &link : VisibleBodyLinks(message, region, true)) {
				add(link.rect, true, link.handler, message->textItem()->fullId());
			}
		}
		const auto step = std::max(st::lineWidth * 8, 4);
		for (auto y = region.top() + step / 2; y <= region.bottom(); y += step) {
			for (auto x = region.left() + step / 2; x <= region.right(); x += step) {
				const auto point = QPoint(x, y);
				const auto state = element->textState(point, {});
				add(QRect(point, QSize(1, 1)), false, state.link, state.itemId ? state.itemId : id);
			}
		}
		return true;
	};
	if (view.main) {
		view.main->purpleEnumerateVisible(collect);
	} else {
		view.section->purpleEnumerateVisible(collect);
	}
}

WindowHintTarget::Menu MessageHints::buildMenu(
		Target view,
		not_null<HistoryView::Element*> element,
		QPoint local) {
	if (view.session().frozen()) {
		return {};
	}
	const auto item = element->data();
	if (const auto main = view.main) {
		const auto previous = main->_dragStateItem;
		const auto guard = QPointer(main);
		auto event = MessageHintMenuEvent(item, local, main->mapToGlobal(local), true);
		main->showContextMenu(&event);
		if (guard) {
			main->_dragStateItem = previous;
		}
		return std::move(event.menu);
	} else if (const auto controller = view.section->controllerOrNull()) {
		auto request = HistoryView::ContextMenuRequest(controller);
		request.view = element;
		request.item = item;
		request.pointState = HistoryView::PointState::Inside;
		request.passive = true;
		return HistoryView::FillContextMenu(view.section, request);
	}
	return {};
}

MessageHints::Input MessageHints::typeWindow(
		int key,
		Ui::RpWidget *root,
		Main::Session *session,
		Fn<void(WindowHintTarget)> showMenu) {
	if (!windowActive() || root != _windowRoot || session != _session.get()
		|| WindowHintClip(root) != _globalClip) {
		clear();
		return Input::Finished;
	}
	if (key == Qt::Key_Backspace || key == Qt::Key_Delete) {
		_prefix.chop(1);
		_overlay->update();
		return Input::Pending;
	} else if (key < Qt::Key_A || key > Qt::Key_Z) {
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
	auto entry = std::move(_windowEntries[i - begin(_entries)]);
	const auto menus = _windowMenus;
	clear();
	if (!entry.current() || !WindowHintExposed(entry.widget, entry.bounds.center())) {
		return Input::Finished;
	}
	if (menus) {
		showMenu(std::move(entry));
		return Input::Finished;
	}
	entry.activate();
	return entry.reply ? Input::LeaveNormal : Input::Finished;
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
	if (key == Qt::Key_Backspace || key == Qt::Key_Delete) {
		_prefix.chop(1);
		_overlay->update();
		return Input::Pending;
	} else if (key < Qt::Key_A || key > Qt::Key_Z) {
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
	if (state.itemId != link.owner || !MatchesUrl(state.link, link.url)) {
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
	_windowRoot = nullptr;
	_windowMenus = false;
	delete _overlay.data();
	_view = nullptr;
	_session = {};
	_entries.clear();
	_windowEntries.clear();
	_prefix.clear();
	_globalClip = {};
}

} // namespace Purple

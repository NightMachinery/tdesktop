#include "purple/purple_hint_targets.h"

#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "dialogs/dialogs_inner_widget.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "history/history_item.h"
#include "info/media/info_media_list_section.h"
#include "info/media/info_media_list_widget.h"
#include "overview/overview_layout.h"
#include "ui/abstract_button.h"
#include "ui/controls/send_button.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_peer_menu.h"

#include "styles/style_chat.h"

#include <QtGui/QMouseEvent>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>

#include <optional>

namespace Purple {

class HintDialogRows {
public:
	static void Append(
		Dialogs::InnerWidget *list,
		QRect clip,
		std::vector<WindowHintTarget> &out) {
		const auto guard = QPointer(list);
		const auto origin = list->mapToGlobal(QPoint());
		const auto filter = list->_filterId;
		for (auto index = 0, count = list->accessibilityChildCount(); index < count; ++index) {
			const auto bounds = list->accessibilityChildRect(index).translated(origin).intersected(clip);
			if (bounds.isEmpty()) {
				continue;
			}
			const auto identity = At(list, index);
			if (!identity.token) {
				continue;
			}
			const auto current = [=] {
				return guard && filter == guard->_filterId && At(guard, index) == identity
					&& guard->accessibilityChildRect(index).translated(guard->mapToGlobal(QPoint()))
						.intersected(WindowHintClip(guard)) == bounds;
			};
			out.push_back({
				.widget = list,
				.bounds = bounds,
				.name = list->accessibilityChildName(index),
				.mainList = true,
				.kind = WindowHintTarget::Kind::Chat,
				.current = current,
				.activate = [=] {
					if (guard->selectChildByIndex(index) && guard && filter == guard->_filterId
						&& At(guard, index) == identity) {
						guard->chooseRow();
					}
				},
				.nativeMenu = [=] {
					const auto key = Key(guard, index);
					if (!key) {
						return WindowHintTarget::Menu();
					}
					auto menu = base::make_unique_q<Ui::PopupMenu>(guard, st::popupMenuExpandedSeparator);
					Window::FillDialogsEntryMenu(guard->_controller, {
						.key = key,
					.section = Dialogs::EntryState::Section::ContextMenu,
						.filterId = filter,
					}, Ui::Menu::CreateAddActionCallback(menu));
					return menu;
				},
			});
		}
	}

private:
	struct Identity {
		quintptr token = 0;
		PeerId peer;
		MsgId topic;
		FullMsgId message;
		friend bool operator==(const Identity &, const Identity &) = default;
	};

	static Identity At(Dialogs::InnerWidget *list, int index) {
		auto result = Identity{ .token = list->accessibilityChildIdentity(index) };
		auto key = Key(list, index);
		if (const auto ref = list->filteredChildAt(index);
			list->_state == Dialogs::WidgetState::Filtered && ref) {
			using Cohort = Dialogs::InnerWidget::AccessibilityCohort;
			switch (ref->cohort) {
			case Cohort::Hashtag: break;
			case Cohort::Filtered: break;
			case Cohort::PeerSearch: return {};
			case Cohort::Preview:
			case Cohort::Searched: {
				const auto item = (ref->cohort == Cohort::Preview)
					? list->_previewResults[ref->local]->item()
					: list->_searchResults[ref->local]->item();
				key = item->history();
				result.message = item->fullId();
			} break;
			}
		}
		if (const auto history = key.owningHistory()) {
			if (history->purpleHiddenFromView()) {
				return {};
			}
			result.peer = history->peer->id;
		}
		if (const auto topic = key.topic()) {
			result.topic = topic->rootId();
		}
		return result;
	}

	static Dialogs::Key Key(Dialogs::InnerWidget *list, int index) {
		if (list->_state == Dialogs::WidgetState::Default) {
			const auto ref = list->defaultChildAt(index);
			if (ref) {
				return ref->row ? ref->row->key() : list->purpleHintCollapsedRow(ref->collapsed);
			}
		} else if (list->_state == Dialogs::WidgetState::Filtered) {
			const auto ref = list->filteredChildAt(index);
			if (ref && ref->cohort == Dialogs::InnerWidget::AccessibilityCohort::Filtered) {
				return list->_filterResults[ref->local].key();
			}
		}
		return {};
	}
};

class HintMediaRows {
public:
	static void Append(
		Info::Media::ListWidget *list,
		QRect clip,
		std::vector<WindowHintTarget> &out) {
		const auto guard = QPointer(list);
		const auto origin = list->mapToGlobal(QPoint());
		for (const auto &section : list->_sections) {
			if (section.top() + origin.y() > clip.bottom()
				|| section.bottom() + origin.y() < clip.top()) {
				continue;
			}
			for (const auto layout : section.items()) {
				const auto item = layout->getItem();
				if (item->history()->purpleHiddenFromView()) {
					continue;
				}
				const auto bounds = section.findItemDetails(layout).geometry
					.translated(origin + QPoint(0, section.top())).intersected(clip);
				if (bounds.isEmpty()) {
					continue;
				}
				const auto id = item->fullId();
				const auto local = bounds.center() - origin;
				out.push_back({
					.widget = list,
					.bounds = bounds,
					.current = [=] {
						if (!guard || guard->_sections.empty() || guard->mapToGlobal(QPoint()) != origin) {
							return false;
						}
						const auto live = guard->findItemByPoint(local);
						return live.exact && live.layout == layout
							&& live.layout->getItem()->fullId() == id
							&& !live.layout->getItem()->history()->purpleHiddenFromView();
					},
					.activate = [=] { ClickWindowHint(list, local); },
				});
			}
		}
	}
};

namespace {

class FolderHintMenu final : public QObject {
public:
	FolderHintMenu(QWidget *button, WindowHintTarget::MenuBuilder builder)
	: QObject(button)
	, build(std::move(builder)) {
	}

	WindowHintTarget::MenuBuilder build;
};

void AddButton(QWidget *widget, QRect clip, std::vector<WindowHintTarget> &out) {
	const auto button = dynamic_cast<Ui::AbstractButton*>(widget);
	const auto qt = qobject_cast<QAbstractButton*>(widget);
	if ((!button && !qt) || (button && button->isDisabled())) {
		return;
	}
	const auto send = dynamic_cast<Ui::SendButton*>(button);
	const auto sendState = send ? std::optional(send->state()) : std::nullopt;
	if (sendState && (sendState->type == Ui::SendButton::Type::Record
		|| sendState->type == Ui::SendButton::Type::Round)) {
		return;
	}
	const auto guard = QPointer(widget);
	const auto name = widget->accessibleName().isEmpty()
		? (button ? button->accessibilityName() : qt->text())
		: widget->accessibleName();
	const auto folder = ranges::find_if(widget->children(), [](QObject *child) {
		return dynamic_cast<FolderHintMenu*>(child) != nullptr;
	});
	const auto folderMenu = (folder == widget->children().end())
		? nullptr : static_cast<FolderHintMenu*>(*folder);
	out.push_back({
		.widget = widget,
		.bounds = clip,
		.name = name,
		.kind = folderMenu ? WindowHintTarget::Kind::Folder : WindowHintTarget::Kind::Control,
		.current = [=] {
			return guard && (!button || !button->isDisabled())
				&& (!send || send->state() == *sendState);
		},
		.activate = [=] {
			if (button) {
				button->clicked(Qt::NoModifier, Qt::LeftButton);
			} else {
				qt->click();
			}
		},
		.nativeMenu = folderMenu ? folderMenu->build : WindowHintTarget::MenuBuilder(),
	});
}

} // namespace

void RegisterFolderHintMenu(QWidget *button, WindowHintTarget::MenuBuilder builder) {
	new FolderHintMenu(button, std::move(builder));
}

QRect WindowHintClip(QWidget *widget) {
	if (!widget || !widget->isVisible() || !widget->isEnabled()) {
		return {};
	}
	auto clip = QRect(widget->mapToGlobal(QPoint()), widget->size());
	for (auto parent = widget->parentWidget(); parent; parent = parent->parentWidget()) {
		clip &= QRect(parent->mapToGlobal(QPoint()), parent->size());
		if (parent->isWindow() || clip.isEmpty()) {
			break;
		}
	}
	return clip;
}

bool WindowHintExposed(QWidget *widget, QPoint globalPoint) {
	if (!WindowHintClip(widget).contains(globalPoint)) {
		return false;
	}
	const auto window = widget->window();
	const auto hit = window->childAt(window->mapFromGlobal(globalPoint));
	return hit && (hit == widget || widget->isAncestorOf(hit));
}

void ClickWindowHint(QWidget *widget, QPoint localPoint) {
	const auto global = widget->mapToGlobal(localPoint);
	const auto guard = QPointer(widget);
	auto press = QMouseEvent(QEvent::MouseButtonPress, localPoint, global,
		Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(widget, &press);
	if (guard) {
		auto release = QMouseEvent(QEvent::MouseButtonRelease, localPoint, global,
			Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
		QApplication::sendEvent(widget, &release);
	}
}

std::vector<WindowHintTarget> CollectWindowHintTargets(QWidget *root) {
	auto result = std::vector<WindowHintTarget>();
	auto widgets = root->findChildren<QWidget*>();
	widgets.push_front(root);
	for (const auto widget : widgets) {
		const auto clip = WindowHintClip(widget);
		if (widget->window() != root->window() || clip.isEmpty()) {
			continue;
		}
		if (const auto source = dynamic_cast<WindowHintSource*>(widget)) {
			source->collectWindowHints(clip, result);
		} else if (const auto dialogs = dynamic_cast<Dialogs::InnerWidget*>(widget)) {
			HintDialogRows::Append(dialogs, clip, result);
		} else if (const auto media = dynamic_cast<Info::Media::ListWidget*>(widget)) {
			HintMediaRows::Append(media, clip, result);
		} else {
			AddButton(widget, clip, result);
		}
	}
	return result;
}

} // namespace Purple

#include "purple/purple_hint_targets.h"
#include "purple/purple_hint_item_identity.h"

#include "data/data_document.h"
#include "data/data_file_click_handler.h"
#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "dialogs/dialogs_inner_widget.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_cursor_state.h"
#include "info/media/info_media_list_section.h"
#include "info/media/info_media_list_widget.h"
#include "overview/overview_layout.h"
#include "ui/abstract_button.h"
#include "ui/rect.h"
#include "ui/controls/send_button.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_peer_menu.h"

#include "styles/style_chat.h"
#include "styles/style_layers.h"
#include "styles/style_purple_music.h"
#include "styles/style_media_player.h"
#include "media/player/media_player_button.h"
#include "ui/widgets/buttons.h"
#include <QtWidgets/QDialogButtonBox>

#include <QtGui/QMouseEvent>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QAbstractItemView>
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
				.role = HintRole::Navigate,
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
				const auto cursor = local - list->findItemByPoint(local).geometry.topLeft();
				const auto handler = layout->getState(cursor, {}).link;
				out.push_back({
					.widget = list,
					.bounds = bounds,
					.role = WindowHintHandlerRole(handler),
					.current = [=] {
						if (!guard || guard->_sections.empty() || guard->mapToGlobal(QPoint()) != origin) {
							return false;
						}
						const auto live = guard->findItemByPoint(local);
						return live.exact && live.layout == layout
							&& layout->getState(local - live.geometry.topLeft(), {}).link == handler
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

void AddItemView(QAbstractItemView *view, std::vector<WindowHintTarget> &out) {
	const auto viewport = view->viewport();
	const auto origin = viewport->mapToGlobal(QPoint());
	const auto visible = WindowHintClip(viewport).translated(-origin);
	auto seen = QSet<QPersistentModelIndex>();
	const auto step = std::max(1, st::lineWidth * 8);
	for (auto y = visible.top(); y <= visible.bottom(); y += step) {
		for (auto x = visible.left(); x <= visible.right(); x += step) {
			const auto index = QPersistentModelIndex(view->indexAt(QPoint(x, y)));
			if (!index.isValid() || seen.contains(index) || !index.flags().testFlag(Qt::ItemIsEnabled)) {
				continue;
			}
			seen.insert(index);
			const auto row = view->visualRect(index).intersected(visible);
			if (row.isEmpty()) {
				continue;
			}
			const auto local = row.center();
			const auto identity = HintItemIdentity{ view, viewport, index, origin, local };
			out.push_back({
				.widget = viewport,
				.bounds = row.translated(origin),
				.name = index.data().toString(),
				.current = [=] { return identity.current(); },
				.activate = [=] { ClickWindowHint(viewport, local); },
			});
		}
	}
}

class FolderHintMenu final : public QObject {
public:
	FolderHintMenu(QWidget *button, WindowHintTarget::MenuBuilder builder)
	: QObject(button)
	, build(std::move(builder)) {
	}

	WindowHintTarget::MenuBuilder build;
};

HintRole ButtonRole(QWidget *widget) {
	const auto tagged = TaggedHintRole(widget, HintRole::Count);
	if (tagged != HintRole::Count) {
		return tagged;
	} else if (dynamic_cast<Media::Player::PlayButton*>(widget)) {
		return HintRole::PlayPause;
	} else if (dynamic_cast<Media::Player::SpeedButton*>(widget)) {
		return HintRole::Playback;
	} else if (const auto icon = dynamic_cast<Ui::IconButton*>(widget)) {
		const auto style = &icon->st();
		if (style == &st::mediaPlayerClose || style == &st::boxTitleClose || style == &st::separatePanelClose) {
			return HintRole::Close;
		} else if (style == &st::purpleMusicMore || style == &st::boxTitleMenu) {
			return HintRole::Menu;
		} else if (style == &st::purpleMusicBack || style == &st::purpleMusicBackRtl) {
			return HintRole::Navigate;
		} else if (style == &st::mediaPlayerPreviousButton || style == &st::mediaPlayerNextButton
			|| style == &st::mediaPlayerVolumeToggle || style == &st::mediaPlayerRepeatButton
			|| style == &st::mediaPlayerOrderButton || style == &st::purpleMusicSkipButton) {
			return HintRole::Playback;
		}
	} else if (const auto round = dynamic_cast<Ui::RoundButton*>(widget)) {
		if (&round->st() == &st::attentionBoxButton) {
			return HintRole::Destructive;
		}
	} else if (const auto button = qobject_cast<QAbstractButton*>(widget)) {
		if (const auto box = qobject_cast<QDialogButtonBox*>(widget->parentWidget())) {
			const auto role = box->buttonRole(button);
			if (role == QDialogButtonBox::DestructiveRole) return HintRole::Destructive;
			if (role == QDialogButtonBox::RejectRole) return HintRole::Close;
		}
	}
	return HintRole::General;
}

QRect ButtonFace(QWidget *widget, QRect clip) {
	auto local = widget->rect();
	if (const auto button = dynamic_cast<Ui::IconButton*>(widget)) {
		const auto &st = button->st();
		if (st.icon.empty()) return clip;
		const auto size = st.icon.size();
		const auto pos = st.iconPosition;
		const auto x = pos.x() < 0 ? (local.width() - size.width()) / 2 : pos.x();
		const auto y = pos.y() < 0 ? (local.height() - size.height()) / 2 : pos.y();
		local = style::rtlrect(x, y, size.width(), size.height(), local.width());
	} else if (dynamic_cast<Media::Player::PlayButton*>(widget)) {
		local = local.marginsRemoved(QMargins(local.width() / 4, local.height() / 4,
			local.width() / 4, local.height() / 4));
	} else {
		return clip;
	}
	const auto face = local.translated(widget->mapToGlobal(QPoint())).intersected(clip);
	return face.isEmpty() ? clip : face;
}

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
		.bounds = ButtonFace(widget, clip),
		.name = name,
		.kind = folderMenu ? WindowHintTarget::Kind::Folder : WindowHintTarget::Kind::Control,
		.role = folderMenu ? HintRole::Navigate : ButtonRole(widget),
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

HintRole WindowHintHandlerRole(const ClickHandlerPtr &handler) {
	if (!handler) {
		return HintRole::General;
	} else if (const auto doc = dynamic_cast<DocumentClickHandler*>(handler.get())) {
		if (doc->document()->isAudioFile() || doc->document()->isVoiceMessage()) {
			if (dynamic_cast<DocumentSaveClickHandler*>(doc) || dynamic_cast<VoiceSeekClickHandler*>(doc)) {
				return HintRole::Playback;
			} else if (dynamic_cast<DocumentOpenClickHandler*>(doc)) {
				return HintRole::PlayPause;
			}
		} else if (doc->document()->isVideoFile() && dynamic_cast<DocumentOpenClickHandler*>(doc)) {
			return HintRole::Navigate;
		}
	} else if (dynamic_cast<PhotoOpenClickHandler*>(handler.get())) {
		return HintRole::Navigate;
	}
	switch (handler->getTextEntity().type) {
	case EntityType::Url:
	case EntityType::CustomUrl:
	case EntityType::Mention:
	case EntityType::MentionName:
	case EntityType::Hashtag:
	case EntityType::Cashtag: return HintRole::Navigate;
	case EntityType::MediaTimestamp: return HintRole::Playback;
	default: return HintRole::General;
	}
}

void SetWindowHintRole(QObject *object, HintRole role) {
	object->setProperty("purpleHintRole", int(role));
}

HintRole TaggedHintRole(const QObject *object, HintRole fallback) {
	const auto value = object->property("purpleHintRole");
	const auto role = value.toInt();
	return (value.isValid() && role >= 0 && role < int(HintRole::Count))
		? HintRole(role)
		: fallback;
}

void RegisterFolderHintMenu(QWidget *button, WindowHintTarget::MenuBuilder builder) {
	new FolderHintMenu(button, std::move(builder));
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
		} else if (const auto view = qobject_cast<QAbstractItemView*>(widget)) {
			AddItemView(view, result);
		} else {
			AddButton(widget, clip, result);
		}
	}
	return result;
}

} // namespace Purple

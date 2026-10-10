#include "purple/purple_folder_strip.h"

#include "data/data_premium_limits.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "purple/purple_gate.h"
#include "purple/purple_preset_box.h"
#include "ui/layers/generic_box.h"
#include "ui/rp_widget.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"

#include <crl/common/crl_common_on_main_guarded.h>
#include <crl/qt/crl_qt_guards.h>
#include <range/v3/algorithm/find.hpp>
#include <rpl/merge.h>

namespace Purple {

bool UseAllFilterIcon(FilterId id) {
	return Data::IsPurpleView(id);
}

FilterId HomeFilterId(not_null<Main::Session*> session) {
	return ActiveResolved().normal
		? FilterId()
		: session->data().chatsFilters().defaultId();
}

const std::vector<Data::ChatFilter> &ShownList(
		not_null<Main::Session*> session) {
	return session->data().chatsFilters().purpleShownList();
}

bool PinWholeStripIfRestricted(not_null<Main::Session*>) {
	return FoldersRestricted();
}

rpl::producer<> FilterStripChanges(not_null<Main::Session*> session) {
	return rpl::merge(
		session->data().chatsFilters().changed(),
		ActiveChanges());
}

namespace {

bool AddViewMenu(
		not_null<Window::SessionController*> controller,
		FilterId id,
		not_null<Ui::PopupMenu*> menu,
		not_null<Ui::RpWidget*> parent) {
	if (!Data::IsPurpleView(id)) {
		return false;
	}
	const auto session = &controller->session();
	const auto addAction = Ui::Menu::CreateAddActionCallback(menu);
	const auto index = Data::PurpleViewIndex(id);
	Window::MenuAddMarkAsReadChatListAction(
		controller,
		[=] { return session->data().purpleViewList(index); },
		addAction);
	addAction(
		u"Work Mode"_q,
		crl::guard(parent.get(), [=] {
			controller->show(Box(PresetBox, session));
		}),
		&st::menuIconEdit);
	return true;
}

} // namespace

bool ShowViewTabMenu(
		not_null<Window::SessionController*> controller,
		FilterId id,
		not_null<Ui::PopupMenu*> menu,
		not_null<Ui::RpWidget*> parent) {
	return AddViewMenu(controller, id, menu, parent);
}

bool FillViewMenu(
		not_null<Window::SessionController*> controller,
		FilterId id,
		not_null<Ui::PopupMenu*> menu,
		not_null<Ui::RpWidget*> parent) {
	return AddViewMenu(controller, id, menu, parent);
}

bool CheckOpenedView(
		not_null<Window::SessionController*> controller) {
	if (ActiveResolved().normal) {
		return false;
	}
	const auto session = &controller->session();
	const auto id = controller->activeChatsFilterCurrent();
	const auto &list = ShownList(session);
	const auto i = ranges::find(list, id, &Data::ChatFilter::id);
	if (i != end(list)) {
		return Data::IsPurpleView(id);
	}
	controller->setActiveChatsFilter(
		HomeFilterId(session),
		{ anim::type::normal, anim::activation::background });
	return true;
}

bool CheckAndJumpToNearChatsFilter(
		not_null<Window::SessionController*> controller,
		bool isNext,
		bool jump) {
	const auto id = controller->activeChatsFilterCurrent();
	const auto session = &controller->session();
	const auto &list = ShownList(session);
	const auto index = int(ranges::find(
		list,
		id,
		&Data::ChatFilter::id
	) - begin(list));
	if (index == int(list.size()) && id != 0) {
		return false;
	}
	const auto changed = index + (isNext ? 1 : -1);
	if (changed >= int(list.size()) || changed < 0) {
		return false;
	}
	if (changed > Data::PremiumLimits(session).dialogFiltersCurrent()) {
		return false;
	}
	if (jump) {
		controller->setActiveChatsFilter((changed >= 0)
			? list[changed].id()
			: 0);
	}
	return true;
}

void EscapeToHome(not_null<Window::SessionController*> controller) {
	const auto home = HomeFilterId(&controller->session());
	if (controller->activeChatsFilterCurrent() != home) {
		controller->setActiveChatsFilter(home);
	}
}

} // namespace Purple

namespace Data {

ChatFilter ChatFilters::purpleViewFilter(int index) const {
	// Named after the preset, or after the view, because that is the honest
	// label: the tab is what this preset shows, and the user picked the name.
	// Nothing else about it is a folder - no rules, no members - which is why
	// every icon lookup falls through to the All chats one and every edit path
	// refuses it.
	const auto title = index
		? Purple::ExtraViews()[index - 1].name
		: Purple::ViewName();
	return ChatFilter(
		PurpleViewFilterId(index),
		{ TextWithEntities{ title } },
		QString(),
		std::nullopt,
		ChatFilter::Flags(),
		{},
		{},
		{});
}

int ChatFilters::purpleViewCount() const {
	return _purpleViewCount;
}

void ChatFilters::purpleRefreshShown() {
	if (!Purple::Filtering()) {
		_purpleShown.clear();
		_purpleViewCount = 0;
		return;
	}
	auto result = std::vector<ChatFilter>();

	// The preset's main view stands where All chats stands, and All chats itself
	// is dropped. That is what makes the whole design work: the main list keeps
	// every chat, so nothing has to be torn out of it, and the one place the
	// user would notice the difference is the one place a preset is meant to
	// change. Left on its own the view takes has() below the threshold and the
	// folder UI disappears by itself, which is what "folders = []" asks for.
	//
	// Then the extra views, in file order, before any folder: they are the
	// preset's own tabs, and a preset's tabs belong next to each other rather
	// than scattered through the account's folders.
	const auto extra = int(Purple::ExtraViews().size());
	_purpleViewCount = std::min(extra + 1, kPurpleViewLimit);
	if (extra + 1 > kPurpleViewLimit) {
		// Silently drawing the first fifteen would read as "the rest are empty",
		// which is the one thing it does not mean.
		LOG(("Purple: preset '%1' defines %2 extra views; showing %3."
			).arg(Purple::ActiveResolved().preset
			).arg(extra
			).arg(kPurpleViewLimit - 1));
	}
	for (auto i = 0; i != _purpleViewCount; ++i) {
		result.push_back(purpleViewFilter(i));
	}

	const auto &shown = Purple::ShownFolders();
	auto missing = QStringList();
	for (const auto &wanted : shown) {
		if (Purple::IsAllFolders(wanted)) {
			// Every folder the selection does not name elsewhere, in the
			// account's own order, at this position. The parser cannot expand
			// it - it has never heard of a Telegram folder - so it arrives here
			// as a marker and is expanded in place, which is what keeps its
			// position in the strip meaningful.
			for (const auto &filter : _list) {
				if (!filter.id()) {
					continue;
				}
				const auto named = ranges::any_of(shown, [&](
						const Purple::PresetFolder &entry) {
					return !Purple::IsAllFolders(entry)
						&& !filter.title().text.text.compare(
							entry.name,
							Qt::CaseInsensitive);
				});
				if (!named
					&& !ranges::contains(result, filter.id(), &ChatFilter::id)) {
					result.push_back(filter);
				}
			}
			continue;
		} else if (!Purple::FolderEnabled(wanted)) {
			// Switched off. Still named, so the "*ALL" above skipped it rather
			// than handing it back, which is the whole reason a disabled entry
			// stays in the resolution instead of being filtered out of it.
			continue;
		} else if (!wanted.show.value_or(true)) {
			// Named, but deliberately not on the strip. Worth being able to say
			// separately from leaving it out: a folder can be silenced or fed
			// into the main view without its tab being there.
			continue;
		}
		const auto i = ranges::find_if(_list, [&](const ChatFilter &filter) {
			return filter.id()
				&& !filter.title().text.text.compare(
					wanted.name,
					Qt::CaseInsensitive);
		});
		if (i == end(_list)) {
			missing.push_back(wanted.name);
		} else if (!ranges::contains(result, i->id(), &ChatFilter::id)) {
			result.push_back(*i);
		}
	}
	if (!missing.isEmpty()) {
		// Same reasoning as the hidden-chat count: a folder name that matches
		// nothing looks identical to a folder the preset meant to hide.
		LOG(("Purple: preset names folders that do not exist: %1."
			).arg(missing.join(u", "_q)));
	}
	LOG(("Purple: folder strip showing %1 of %2, including %3 preset view(s)."
		).arg(result.size()).arg(_list.size()).arg(_purpleViewCount));
	_purpleShown = std::move(result);
}

const std::vector<ChatFilter> &ChatFilters::purpleShownList() const {
	return Purple::Filtering() ? _purpleShown : _list;
}

} // namespace Data

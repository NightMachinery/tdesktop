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

#pragma once

#include "base/basic_types.h"
#include "data/data_chat_filters.h"
#include "rpl/producer.h"

#include <vector>

namespace Dialogs {
struct UnreadState;
} // namespace Dialogs

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class PopupMenu;
class RpWidget;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Purple {

[[nodiscard]] bool UseAllFilterIcon(FilterId id);
[[nodiscard]] FilterId HomeFilterId(not_null<Main::Session*> session);
[[nodiscard]] const std::vector<Data::ChatFilter> &ShownList(
	not_null<Main::Session*> session);
[[nodiscard]] bool PinWholeStripIfRestricted(
	not_null<Main::Session*> session);
[[nodiscard]] bool RefuseFolderOrderSave(
	not_null<Main::Session*> session);
[[nodiscard]] bool QuietFolderUnread(
	not_null<Main::Session*> session,
	FilterId filterId);
[[nodiscard]] rpl::producer<Dialogs::UnreadState> ViewUnreadStateValue(
	not_null<Main::Session*> session,
	FilterId filterId);
[[nodiscard]] rpl::producer<> FilterStripChanges(
	not_null<Main::Session*> session);
[[nodiscard]] bool ShowViewTabMenu(
	not_null<Window::SessionController*> controller,
	FilterId id,
	not_null<Ui::PopupMenu*> menu,
	not_null<Ui::RpWidget*> parent);
[[nodiscard]] bool FillViewMenu(
	not_null<Window::SessionController*> controller,
	FilterId id,
	not_null<Ui::PopupMenu*> menu,
	not_null<Ui::RpWidget*> parent);
[[nodiscard]] bool CheckOpenedView(
	not_null<Window::SessionController*> controller);
[[nodiscard]] bool CheckAndJumpToNearChatsFilter(
	not_null<Window::SessionController*> controller,
	bool isNext,
	bool jump);
void EscapeToHome(not_null<Window::SessionController*> controller);

} // namespace Purple

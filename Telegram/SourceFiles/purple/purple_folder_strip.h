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

class FolderStrip final {
public:
	explicit FolderStrip(not_null<Data::ChatFilters*> filters);
	void subscribe();
	[[nodiscard]] static const std::vector<Data::ChatFilter> &ShownList(
		const Data::ChatFilters *filters);
	[[nodiscard]] static int ViewCount(const Data::ChatFilters *filters);

private:
	[[nodiscard]] Data::ChatFilter viewFilter(int index) const;
	void refreshShown();

	not_null<Data::ChatFilters*> _filters;
	std::vector<Data::ChatFilter> _shown;
	int _viewCount = 0;
	rpl::lifetime _lifetime;
};

[[nodiscard]] bool UseAllFilterIcon(FilterId id);
[[nodiscard]] FilterId HomeFilterId(not_null<Main::Session*> session);
[[nodiscard]] const std::vector<Data::ChatFilter> &ShownList(
	not_null<Main::Session*> session);
[[nodiscard]] const std::vector<Data::ChatFilter> &ShownList(
	not_null<const Data::ChatFilters*> filters);
[[nodiscard]] int ViewCount(not_null<const Data::ChatFilters*> filters);
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
